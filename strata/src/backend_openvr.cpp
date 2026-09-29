#include "strata/backend/openvr.hpp"

#include <windows.h>

#include <openvr.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace strata {

namespace {

// pose -> HmdMatrix34_t: column 0/1/2 are the local x/y/z axes expressed in world space (openvr's convention,
// -z forward like openxr), column 3 is the position.
[[nodiscard]] vr::HmdMatrix34_t pose_to_matrix(const vr_pose& p) noexcept
{
    const vr_quat& q = p.orientation;
    vr::HmdMatrix34_t m{};
    m.m[0][0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    m.m[0][1] = 2.0f * (q.x * q.y - q.w * q.z);
    m.m[0][2] = 2.0f * (q.x * q.z + q.w * q.y);
    m.m[1][0] = 2.0f * (q.x * q.y + q.w * q.z);
    m.m[1][1] = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    m.m[1][2] = 2.0f * (q.y * q.z - q.w * q.x);
    m.m[2][0] = 2.0f * (q.x * q.z - q.w * q.y);
    m.m[2][1] = 2.0f * (q.y * q.z + q.w * q.x);
    m.m[2][2] = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    m.m[0][3] = p.position.x;
    m.m[1][3] = p.position.y;
    m.m[2][3] = p.position.z;
    return m;
}

// the inverse (Shepperd's method): HmdMatrix34_t -> pose
[[nodiscard]] vr_pose matrix_to_pose(const vr::HmdMatrix34_t& m) noexcept
{
    vr_pose p;
    p.position = {m.m[0][3], m.m[1][3], m.m[2][3]};

    vr_quat q;
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m.m[2][1] - m.m[1][2]) / s;
        q.y = (m.m[0][2] - m.m[2][0]) / s;
        q.z = (m.m[1][0] - m.m[0][1]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25f * s;
    }
    p.orientation = q;
    return p;
}

constexpr vr::ETrackedControllerRole controller_roles[max_vr_controllers] = {
    vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand};

// SteamVR Input System manifest: aim uses each model's calibrated pointing pose (`pose/tip`) rather than the raw
// tracking pose, whose local -z rarely lines up with where the controller visually points. trigger and stick get
// actions too since GetControllerState can't be trusted once an app is on the input system; it's kept only as a
// last-resort fallback if the manifest or a given action fails to bind.
constexpr const char* action_set_path         = "/actions/main";
constexpr const char* aim_action_paths[2]      = {"/actions/main/in/aim_left", "/actions/main/in/aim_right"};
constexpr const char* trigger_action_paths[2]  = {"/actions/main/in/trigger_left", "/actions/main/in/trigger_right"};
constexpr const char* stick_action_paths[2]    = {"/actions/main/in/stick_left", "/actions/main/in/stick_right"};

constexpr const char* actions_manifest_json = R"json({
  "action_sets": [
    { "name": "/actions/main", "usage": "leftright" }
  ],
  "actions": [
    { "name": "/actions/main/in/aim_left",     "type": "pose" },
    { "name": "/actions/main/in/aim_right",    "type": "pose" },
    { "name": "/actions/main/in/trigger_left",  "type": "boolean" },
    { "name": "/actions/main/in/trigger_right", "type": "boolean" },
    { "name": "/actions/main/in/stick_left",    "type": "vector2" },
    { "name": "/actions/main/in/stick_right",   "type": "vector2" }
  ],
  "default_bindings": [
    { "controller_type": "vive_controller",       "binding_url": "strata_bindings_vive_controller.json" },
    { "controller_type": "knuckles",               "binding_url": "strata_bindings_knuckles.json" },
    { "controller_type": "oculus_touch",           "binding_url": "strata_bindings_oculus_touch.json" },
    { "controller_type": "holographic_controller", "binding_url": "strata_bindings_holographic_controller.json" },
    { "controller_type": "generic",                "binding_url": "strata_bindings_generic.json" }
  ]
})json";

constexpr const char* bindings_template_json = R"json({
  "bindings": {
    "/actions/main": {
      "poses": [
        { "output": "/actions/main/in/aim_left",  "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/main/in/aim_right", "path": "/user/hand/right/pose/tip" }
      ],
      "sources": [
        {
          "path": "/user/hand/left/input/trigger",
          "mode": "button",
          "inputs": { "click": { "output": "/actions/main/in/trigger_left" } }
        },
        {
          "path": "/user/hand/right/input/trigger",
          "mode": "button",
          "inputs": { "click": { "output": "/actions/main/in/trigger_right" } }
        },
        {
          "path": "/user/hand/left/input/%s",
          "mode": "%s",
          "inputs": { "position": { "output": "/actions/main/in/stick_left" } }
        },
        {
          "path": "/user/hand/right/input/%s",
          "mode": "%s",
          "inputs": { "position": { "output": "/actions/main/in/stick_right" } }
        }
      ]
    }
  },
  "controller_type": "%s",
  "description": "strata generated bindings",
  "name": "strata bindings"
})json";

// stick_component/stick_mode: physical name + binding mode of that model's primary 2d input (a vive wand only has
// a trackpad; everything else here has a joystick). a wrong guess just leaves that binding source inactive.
struct binding_file { const char* controller_type; const char* file_name; const char* stick_component; const char* stick_mode; };
constexpr binding_file binding_files[] = {
    {"vive_controller", "strata_bindings_vive_controller.json", "trackpad", "trackpad"},
    {"knuckles", "strata_bindings_knuckles.json", "joystick", "joystick"},
    {"oculus_touch", "strata_bindings_oculus_touch.json", "joystick", "joystick"},
    {"holographic_controller", "strata_bindings_holographic_controller.json", "joystick", "joystick"},
    {"generic", "strata_bindings_generic.json", "joystick", "joystick"},
};

[[nodiscard]] bool write_text_file(const std::string& path, const char* text) noexcept
{
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) { return false; }
    const bool ok = std::fputs(text, fp) >= 0;
    std::fclose(fp);
    return ok;
}

[[nodiscard]] bool write_bindings_file(const std::string& path, const binding_file& f) noexcept
{
    char buf[4096];
    std::snprintf(buf, sizeof(buf), bindings_template_json, f.stick_component, f.stick_mode, f.stick_component,
                 f.stick_mode, f.controller_type);
    return write_text_file(path, buf);
}

// positions/orients/shows a laser-pointer overlay as a thin quad running from `origin` along `direction` for
// `length` metres (the hit distance when the controller is aiming at the panel, or a fixed default otherwise, so
// the beam is always visible even before it reaches anything). `laser`'s own local x axis becomes the beam's long
// axis; y/z are an arbitrary perpendicular pair (the beam is round enough at this thinness that their exact
// orientation doesn't matter, only that they're unit length and mutually perpendicular to x).
void update_laser(vr::IVROverlay* ovl, vr::VROverlayHandle_t laser, bool active, vr::HmdVector3_t origin,
                  vr::HmdVector3_t direction, float length) noexcept
{
    if (ovl == nullptr || laser == vr::k_ulOverlayHandleInvalid) { return; }
    if (!active) { ovl->HideOverlay(laser); return; }

    const vr_vec3 x{direction.v[0], direction.v[1], direction.v[2]};
    vr_vec3       up{0.0f, 1.0f, 0.0f};
    if (std::fabs(x.x * up.x + x.y * up.y + x.z * up.z) > 0.99f) { up = {0.0f, 0.0f, 1.0f}; } // x nearly parallel to up

    const auto cross = [](vr_vec3 a, vr_vec3 b) noexcept {
        return vr_vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const auto normalize = [](vr_vec3 v) noexcept {
        const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return len > 1e-6f ? vr_vec3{v.x / len, v.y / len, v.z / len} : v;
    };
    const vr_vec3 z = normalize(cross(x, up));
    const vr_vec3 y = normalize(cross(z, x));

    vr::HmdMatrix34_t m{};
    m.m[0][0] = x.x; m.m[1][0] = x.y; m.m[2][0] = x.z;
    m.m[0][1] = y.x; m.m[1][1] = y.y; m.m[2][1] = y.z;
    m.m[0][2] = z.x; m.m[1][2] = z.y; m.m[2][2] = z.z;
    m.m[0][3] = origin.v[0] + x.x * length * 0.5f;
    m.m[1][3] = origin.v[1] + x.y * length * 0.5f;
    m.m[2][3] = origin.v[2] + x.z * length * 0.5f;

    ovl->SetOverlayWidthInMeters(laser, length);
    ovl->SetOverlayTransformAbsolute(laser, vr::TrackingUniverseStanding, &m);
    ovl->ShowOverlay(laser);
}

} // namespace

vr_kind detect_vr() noexcept
{
    if (::GetModuleHandleA("openxr_loader.dll") != nullptr) { return vr_kind::openxr; }
    if (::GetModuleHandleA("openvr_api.dll") != nullptr) { return vr_kind::openvr; }
    return vr_kind::none;
}

struct vr_overlay::impl {
    vr::VROverlayHandle_t handle{vr::k_ulOverlayHandleInvalid};
    std::array<vr::VROverlayHandle_t, max_vr_controllers> laser{
        {vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid}};
    bool     owns_init{};
    bool     visible{};
    vr_panel panel{};
    std::array<vr_controller, max_vr_controllers> controllers{};
    char     error[256]{};

    // strata clicks fire on release while hovered; a physical trigger pull can nudge the ray off a small widget
    // for a frame or two mid-hold. update_controllers() papers over that with the last position that genuinely
    // hit the panel, without freezing the live position outright.
    std::array<vec2, max_vr_controllers> hit_uv_last_known{};
    std::array<bool, max_vr_controllers> hit_last_known_valid{};

    // input system state (see actions_manifest_json above); input_ready is false whenever any setup step fails,
    // and update_controllers() then falls back to the raw device pose/legacy button state it always used to use
    // -- degraded (steep, hard-to-aim ray) rather than broken.
    vr::VRActionSetHandle_t action_set{vr::k_ulInvalidActionSetHandle};
    std::array<vr::VRActionHandle_t, max_vr_controllers> aim_action{
        {vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle}};
    std::array<vr::VRActionHandle_t, max_vr_controllers> trigger_action{
        {vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle}};
    std::array<vr::VRActionHandle_t, max_vr_controllers> stick_action{
        {vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle}};
    bool input_ready{};

    void set_error(const char* msg) noexcept
    {
        std::strncpy(error, msg, sizeof(error) - 1);
        error[sizeof(error) - 1] = '\0';
    }
    void set_error_ovl(const char* what, vr::EVROverlayError e) noexcept
    {
        char buf[256];
        const char* name = vr::VROverlay() != nullptr ? vr::VROverlay()->GetOverlayErrorNameFromEnum(e) : "?";
        std::snprintf(buf, sizeof(buf), "%s failed (%s)", what, name);
        set_error(buf);
    }

    // writes the action manifest + one bindings file per controller_type to a temp directory and points
    // SteamVR Input at it. failure just leaves input_ready false (see above), so it's never fatal to create().
    void init_input() noexcept
    {
        vr::IVRInput* inp = vr::VRInput();
        if (inp == nullptr) { return; }

        char temp_path[MAX_PATH]{};
        if (::GetTempPathA(MAX_PATH, temp_path) == 0) { return; }
        const std::string dir = std::string(temp_path) + "strata_openvr_input\\";
        ::CreateDirectoryA(dir.c_str(), nullptr); // ERROR_ALREADY_EXISTS is fine

        for (const binding_file& f : binding_files) {
            if (!write_bindings_file(dir + f.file_name, f)) { return; }
        }
        const std::string manifest_path = dir + "strata_actions.json";
        if (!write_text_file(manifest_path, actions_manifest_json)) { return; }

        if (inp->SetActionManifestPath(manifest_path.c_str()) != vr::VRInputError_None) { return; }
        if (inp->GetActionSetHandle(action_set_path, &action_set) != vr::VRInputError_None) { return; }
        for (std::size_t i = 0; i < max_vr_controllers; ++i) {
            if (inp->GetActionHandle(aim_action_paths[i], &aim_action[i]) != vr::VRInputError_None) { return; }
            if (inp->GetActionHandle(trigger_action_paths[i], &trigger_action[i]) != vr::VRInputError_None) { return; }
            if (inp->GetActionHandle(stick_action_paths[i], &stick_action[i]) != vr::VRInputError_None) { return; }
        }
        input_ready = true;
    }

    // one thin overlay quad per hand, shown/positioned every frame in update_controllers() as a laser pointer --
    // otherwise there is no visual feedback at all for where a controller is aiming (see update_laser() below).
    void init_lasers(const char* key) noexcept
    {
        vr::IVROverlay* ovl = vr::VROverlay();
        if (ovl == nullptr) { return; }

        constexpr u32 tex_w = 256, tex_h = 1;
        std::vector<u8> pixels(std::size_t{tex_w} * tex_h * 4, 0xFF);
        constexpr const char* suffix[max_vr_controllers] = {".laser_left", ".laser_right"};
        for (std::size_t i = 0; i < max_vr_controllers; ++i) {
            const std::string laser_key = std::string(key) + suffix[i];
            if (ovl->CreateOverlay(laser_key.c_str(), laser_key.c_str(), &laser[i]) != vr::VROverlayError_None) {
                laser[i] = vr::k_ulOverlayHandleInvalid;
                continue;
            }
            ovl->SetOverlayRaw(laser[i], pixels.data(), tex_w, tex_h, 4);
            ovl->SetOverlayColor(laser[i], 0.25f, 0.85f, 1.0f);
            ovl->SetOverlayAlpha(laser[i], 0.55f);
        }
    }

    ~impl()
    {
        if (vr::IVROverlay* ovl = vr::VROverlay(); ovl != nullptr) {
            if (handle != vr::k_ulOverlayHandleInvalid) { ovl->DestroyOverlay(handle); }
            for (vr::VROverlayHandle_t h : laser) {
                if (h != vr::k_ulOverlayHandleInvalid) { ovl->DestroyOverlay(h); }
            }
        }
        if (owns_init) { vr::VR_Shutdown(); }
    }
};

vr_overlay::vr_overlay() noexcept = default;
vr_overlay::~vr_overlay()         = default;
vr_overlay::vr_overlay(vr_overlay&&) noexcept            = default;
vr_overlay& vr_overlay::operator=(vr_overlay&&) noexcept = default;

bool vr_overlay::create(const char* key, const char* friendly_name, const vr_panel& panel)
{
    impl_ = std::make_unique<impl>();
    impl_->panel = panel;

    // vr::VROverlay() safely returns null without crashing when nothing in this process has called VR_Init yet
    // (it just fails the underlying VR_GetGenericInterface lookup) -- that failure is exactly the signal used
    // here to tell "a host game already initialised openvr" apart from "nothing has".
    vr::IVROverlay* ovl = vr::VROverlay();
    if (ovl == nullptr) {
        vr::EVRInitError err = vr::VRInitError_None;
        vr::VR_Init(&err, vr::VRApplication_Overlay);
        if (err != vr::VRInitError_None) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "VR_Init failed (%s)", vr::VR_GetVRInitErrorAsEnglishDescription(err));
            impl_->set_error(buf);
            return false;
        }
        impl_->owns_init = true;
        ovl = vr::VROverlay();
    }
    if (ovl == nullptr) {
        impl_->set_error("VR_Init succeeded but IVROverlay is unavailable");
        return false;
    }

    const vr::EVROverlayError oerr = ovl->CreateOverlay(key, friendly_name, &impl_->handle);
    if (oerr != vr::VROverlayError_None) {
        impl_->set_error_ovl("CreateOverlay", oerr);
        if (impl_->owns_init) { vr::VR_Shutdown(); impl_->owns_init = false; }
        impl_->handle = vr::k_ulOverlayHandleInvalid;
        return false;
    }

    set_panel(panel);
    ovl->HideOverlay(impl_->handle);

    impl_->init_input();
    impl_->init_lasers(key);
    return true;
}

void vr_overlay::destroy() noexcept { impl_.reset(); }

bool vr_overlay::owns_init() const noexcept { return impl_ && impl_->owns_init; }

bool vr_overlay::input_system_ready() const noexcept { return impl_ && impl_->input_ready; }

void vr_overlay::set_panel(const vr_panel& panel) noexcept
{
    if (!impl_) { return; }
    impl_->panel = panel;
    if (vr::IVROverlay* ovl = vr::VROverlay(); ovl != nullptr && impl_->handle != vr::k_ulOverlayHandleInvalid) {
        ovl->SetOverlayWidthInMeters(impl_->handle, panel.width_meters);
        const vr::HmdMatrix34_t m = pose_to_matrix(panel.pose);
        if (panel.head_locked) {
            ovl->SetOverlayTransformTrackedDeviceRelative(impl_->handle, vr::k_unTrackedDeviceIndex_Hmd, &m);
        } else {
            ovl->SetOverlayTransformAbsolute(impl_->handle, vr::TrackingUniverseStanding, &m);
        }
    }
}

const vr_panel& vr_overlay::panel() const noexcept { return impl_->panel; }

void vr_overlay::show() noexcept
{
    if (!impl_) { return; }
    if (vr::IVROverlay* ovl = vr::VROverlay(); ovl != nullptr) {
        ovl->ShowOverlay(impl_->handle);
        impl_->visible = true;
    }
}

void vr_overlay::hide() noexcept
{
    if (!impl_) { return; }
    if (vr::IVROverlay* ovl = vr::VROverlay(); ovl != nullptr) {
        ovl->HideOverlay(impl_->handle);
        impl_->visible = false;
    }
}

bool vr_overlay::visible() const noexcept { return impl_ && impl_->visible; }

void vr_overlay::submit_texture(ID3D11Texture2D* texture) noexcept
{
    if (!impl_ || texture == nullptr) { return; }
    vr::IVROverlay* ovl = vr::VROverlay();
    if (ovl == nullptr) { return; }
    vr::Texture_t tex{};
    tex.handle      = texture;
    tex.eType       = vr::TextureType_DirectX;
    tex.eColorSpace = vr::ColorSpace_Auto;
    ovl->SetOverlayTexture(impl_->handle, &tex);
}

void vr_overlay::update_controllers() noexcept
{
    if (!impl_) { return; }
    for (vr_controller& c : impl_->controllers) { c = {}; }

    vr::IVRSystem*  sys = vr::VRSystem();
    vr::IVROverlay* ovl = vr::VROverlay();
    if (sys == nullptr || ovl == nullptr || impl_->handle == vr::k_ulOverlayHandleInvalid) { return; }

    vr::IVRInput* inp = impl_->input_ready ? vr::VRInput() : nullptr;
    if (inp != nullptr) {
        vr::VRActiveActionSet_t aas{};
        aas.ulActionSet = impl_->action_set;
        inp->UpdateActionState(&aas, sizeof(aas), 1);
    }

    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, poses.data(),
                                         static_cast<uint32_t>(poses.size()));

    for (u32 i = 0; i < max_vr_controllers; ++i) {
        vr_controller& c = impl_->controllers[i];

        const vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(controller_roles[i]);
        const bool have_device = idx != vr::k_unTrackedDeviceIndexInvalid && idx < poses.size() &&
                                  poses[idx].bPoseIsValid && poses[idx].bDeviceIsConnected;
        if (!have_device) { update_laser(ovl, impl_->laser[i], false, {}, {}, 0.0f); continue; }

        c.tracked = true;

        // tip pose when available (see actions_manifest_json above), else the raw device pose.
        vr::HmdMatrix34_t aim_matrix = poses[idx].mDeviceToAbsoluteTracking;
        if (inp != nullptr) {
            vr::InputPoseActionData_t pose_data{};
            if (inp->GetPoseActionDataForNextFrame(impl_->aim_action[i], vr::TrackingUniverseStanding, &pose_data,
                                                    sizeof(pose_data),
                                                    vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
                pose_data.bActive && pose_data.pose.bPoseIsValid) {
                aim_matrix   = pose_data.pose.mDeviceToAbsoluteTracking;
                c.aim_from_tip = true;
            }
        }
        c.aim = matrix_to_pose(aim_matrix);

        // fallback only for whichever of trigger/stick the input system couldn't read (see above).
        vr::VRControllerState_t state{};
        const bool have_state = sys->GetControllerState(idx, &state, sizeof(state));

        bool trigger_read = false;
        if (inp != nullptr) {
            vr::InputDigitalActionData_t digital{};
            if (inp->GetDigitalActionData(impl_->trigger_action[i], &digital, sizeof(digital),
                                          vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
                digital.bActive) {
                c.trigger_down = digital.bState;
                trigger_read   = true;
            }
        }
        if (!trigger_read && have_state) {
            c.trigger_down = (state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
        }

        bool stick_read = false;
        if (inp != nullptr) {
            vr::InputAnalogActionData_t analog{};
            if (inp->GetAnalogActionData(impl_->stick_action[i], &analog, sizeof(analog),
                                         vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
                analog.bActive) {
                c.joystick = {analog.x, analog.y};
                stick_read = true;
            }
        }
        if (!stick_read && have_state) { c.joystick = {state.rAxis[0].x, state.rAxis[0].y}; }

        vr::VROverlayIntersectionParams_t params{};
        params.eOrigin    = vr::TrackingUniverseStanding;
        params.vSource    = {aim_matrix.m[0][3], aim_matrix.m[1][3], aim_matrix.m[2][3]};
        params.vDirection = {-aim_matrix.m[0][2], -aim_matrix.m[1][2], -aim_matrix.m[2][2]};
        vr::VROverlayIntersectionResults_t results{};
        float beam_length = 5.0f; // no hit yet: show the beam this far out so it's still visible while aiming
        if (ovl->ComputeOverlayIntersection(impl_->handle, &params, &results)) {
            c.hit       = true;
            c.hit_uv    = {results.vUVs.v[0], results.vUVs.v[1]};
            beam_length = results.fDistance;
        }

        // see impl::hit_uv_last_known: keep tracking the live hit position through a held trigger, but paper
        // over a momentary off-panel miss (trigger-pull wobble) with the last position that genuinely hit.
        if (c.hit) {
            impl_->hit_uv_last_known[i]    = c.hit_uv;
            impl_->hit_last_known_valid[i] = true;
        } else if (c.trigger_down && impl_->hit_last_known_valid[i]) {
            c.hit    = true;
            c.hit_uv = impl_->hit_uv_last_known[i];
        }
        if (!c.trigger_down) { impl_->hit_last_known_valid[i] = false; } // don't leak a stale point into the next press

        update_laser(ovl, impl_->laser[i], true, params.vSource, params.vDirection, beam_length);
    }
}

std::span<const vr_controller> vr_overlay::controllers() const noexcept
{
    return impl_ ? std::span<const vr_controller>{impl_->controllers} : std::span<const vr_controller>{};
}

const char* vr_overlay::last_error() const noexcept { return impl_ ? impl_->error : ""; }

input_state vr_pointer_input(std::span<const vr_controller> controllers, input_state base) noexcept
{
    int active = -1;
    for (std::size_t i = 0; i < controllers.size(); ++i) {
        if (!controllers[i].hit) { continue; }
        const bool prefer = active < 0 ||
                             (controllers[i].trigger_down && !controllers[static_cast<std::size_t>(active)].trigger_down);
        if (prefer) { active = static_cast<int>(i); }
    }

    if (active >= 0) {
        const vr_controller& c = controllers[static_cast<std::size_t>(active)];
        // ComputeOverlayIntersection's uv has v=0 at the overlay's bottom (opengl convention); mouse_pos, like
        // the d3d11/d3d12 backends, expects y=0 at the top.
        base.mouse_pos     = {c.hit_uv.x * base.display_size.x, (1.0f - c.hit_uv.y) * base.display_size.y};
        base.mouse_down[0] = c.trigger_down;
    }
    return base;
}

std::optional<vr_pose> vr_pose_in_front_of_hmd(float distance_m) noexcept
{
    vr::IVRSystem* sys = vr::VRSystem();
    if (sys == nullptr) { return std::nullopt; }

    // a single query, not a retry loop: callers that want the panel to track the hmd every frame (a head-locked
    // panel) can't afford to block here, and one occasional missed frame right after VR_Init self-heals on the
    // next call anyway.
    vr::TrackedDevicePose_t hmd{};
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &hmd, 1); // index 0 is always the hmd
    if (!hmd.bPoseIsValid) { return std::nullopt; }

    const vr::HmdMatrix34_t& m = hmd.mDeviceToAbsoluteTracking;
    vr_vec3 fwd{-m.m[0][2], 0.0f, -m.m[2][2]}; // hmd forward, flattened to the horizontal plane so the panel stays level
    const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (len > 1e-4f) { fwd.x /= len; fwd.z /= len; } else { fwd = {0.0f, 0.0f, -1.0f}; } // looking straight up/down

    vr_pose p;
    p.position = {m.m[0][3] + fwd.x * distance_m, m.m[1][3], m.m[2][3] + fwd.z * distance_m};
    // an overlay quad reads correctly to a viewer standing on its local +z side looking toward -z, so +z is
    // pointed back at the hmd (i.e. -fwd) rather than along fwd.
    const float yaw = std::atan2(-fwd.x, -fwd.z);
    p.orientation   = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    return p;
}

} // namespace strata
