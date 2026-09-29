#include "strata/backend/openxr.hpp"

#include <windows.h>

#include <d3d11.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace strata {

namespace {

constexpr XrPosef identity_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

[[nodiscard]] XrQuaternionf to_xr(xr_quat q) noexcept { return {q.x, q.y, q.z, q.w}; }
[[nodiscard]] XrVector3f    to_xr(xr_vec3 v) noexcept { return {v.x, v.y, v.z}; }
[[nodiscard]] XrPosef       to_xr(const xr_pose& p) noexcept { return {to_xr(p.orientation), to_xr(p.position)}; }
[[nodiscard]] xr_vec3       from_xr(XrVector3f v) noexcept { return {v.x, v.y, v.z}; }
[[nodiscard]] xr_quat       from_xr(XrQuaternionf q) noexcept { return {q.x, q.y, q.z, q.w}; }
[[nodiscard]] xr_pose       from_xr(const XrPosef& p) noexcept { return {from_xr(p.position), from_xr(p.orientation)}; }

[[nodiscard]] xr_vec3 rotate(xr_quat q, xr_vec3 v) noexcept
{
    const xr_vec3 qv{q.x, q.y, q.z};
    const xr_vec3 t{qv.y * v.z - qv.z * v.y, qv.z * v.x - qv.x * v.z, qv.x * v.y - qv.y * v.x};
    const xr_vec3 t2{t.x + v.x * q.w, t.y + v.y * q.w, t.z + v.z * q.w};
    const xr_vec3 c{qv.y * t2.z - qv.z * t2.y, qv.z * t2.x - qv.x * t2.z, qv.x * t2.y - qv.y * t2.x};
    return {v.x + 2.0f * c.x, v.y + 2.0f * c.y, v.z + 2.0f * c.z};
}
[[nodiscard]] xr_vec3 sub(xr_vec3 a, xr_vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] f32     dot(xr_vec3 a, xr_vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

} // namespace

struct xr_session::impl {
    XrInstance  instance{XR_NULL_HANDLE};
    XrSystemId  system{XR_NULL_SYSTEM_ID};
    XrSession   session{XR_NULL_HANDLE};
    XrSpace     space{XR_NULL_HANDLE}; // LOCAL reference space: the panel's pose is relative to this
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<XrSwapchainImageD3D11KHR> swapchain_images;

    XrActionSet                             action_set{XR_NULL_HANDLE};
    XrAction                                aim_action{XR_NULL_HANDLE};
    XrAction                                trigger_action{XR_NULL_HANDLE};
    std::array<XrPath, max_xr_controllers>  hand_paths{};
    std::array<XrSpace, max_xr_controllers> hand_spaces{};

    LUID                    adapter_luid{};
    XrEnvironmentBlendMode  blend_mode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    XrViewConfigurationType view_config{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};

    xr_panel                                      panel{};
    std::array<xr_controller, max_xr_controllers> controller_state{};

    XrSessionState session_state{XR_SESSION_STATE_UNKNOWN};
    bool           running{};            // xrBeginSession called, xrEndSession not yet
    bool           should_render_last{}; // this frame's shouldRender, remembered for end_frame()
    XrTime         predicted_display_time{};

    char error[256]{};

    void set_error(const char* msg) noexcept
    {
        std::strncpy(error, msg, sizeof(error) - 1);
        error[sizeof(error) - 1] = '\0';
    }
    void set_error_xr(const char* what, XrResult r) noexcept
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed (XrResult %d)", what, static_cast<int>(r));
        set_error(buf);
    }

    ~impl()
    {
        if (session != XR_NULL_HANDLE) {
            if (running) { xrEndSession(session); }
            for (XrSpace s : hand_spaces) { if (s != XR_NULL_HANDLE) { xrDestroySpace(s); } }
            if (space != XR_NULL_HANDLE) { xrDestroySpace(space); }
            if (swapchain != XR_NULL_HANDLE) { xrDestroySwapchain(swapchain); }
            if (action_set != XR_NULL_HANDLE) { xrDestroyActionSet(action_set); }
            xrDestroySession(session);
        }
        if (instance != XR_NULL_HANDLE) { xrDestroyInstance(instance); }
    }
};

xr_session::xr_session() noexcept = default;
xr_session::~xr_session()         = default;
xr_session::xr_session(xr_session&&) noexcept            = default;
xr_session& xr_session::operator=(xr_session&&) noexcept = default;

bool xr_session::create_instance(const char* app_name)
{
    impl_ = std::make_unique<impl>();

    // enumerate first so a missing extension fails with a clear message instead of a bare xrCreateInstance error
    uint32_t ext_count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr))) {
        impl_->set_error("no openxr runtime found (is SteamVR / your runtime installed and set active?)");
        return false;
    }
    std::vector<XrExtensionProperties> exts(ext_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());
    bool has_d3d11 = false;
    for (const XrExtensionProperties& e : exts) {
        if (std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) { has_d3d11 = true; break; }
    }
    if (!has_d3d11) {
        impl_->set_error("the active openxr runtime does not support XR_KHR_D3D11_enable");
        return false;
    }

    const char* enabled_exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled_exts;
    std::strncpy(ici.applicationInfo.applicationName, app_name, XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrResult r = xrCreateInstance(&ici, &impl_->instance);
    if (XR_FAILED(r)) {
        impl_->set_error_xr("xrCreateInstance", r);
        return false;
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(impl_->instance, &sgi, &impl_->system);
    if (XR_FAILED(r)) {
        impl_->set_error_xr("xrGetSystem (no headset found)", r);
        return false;
    }

    PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
    xrGetInstanceProcAddr(impl_->instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_reqs));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    r = get_reqs != nullptr ? get_reqs(impl_->instance, impl_->system, &reqs) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(r)) {
        impl_->set_error_xr("xrGetD3D11GraphicsRequirementsKHR", r);
        return false;
    }
    impl_->adapter_luid = reqs.adapterLuid;

    // the runtime's first choice is normally XR_ENVIRONMENT_BLEND_MODE_OPAQUE for a vr headset (as opposed to an
    // ar passthrough device, which would put an additive/alpha-blend mode first)
    uint32_t blend_count = 0;
    xrEnumerateEnvironmentBlendModes(impl_->instance, impl_->system, impl_->view_config, 0, &blend_count, nullptr);
    std::vector<XrEnvironmentBlendMode> blend_modes(blend_count);
    if (blend_count > 0) {
        xrEnumerateEnvironmentBlendModes(impl_->instance, impl_->system, impl_->view_config, blend_count, &blend_count,
                                         blend_modes.data());
        impl_->blend_mode = blend_modes.front();
    }

    return true;
}

const LUID& xr_session::adapter_luid() const noexcept { return impl_->adapter_luid; }

bool xr_session::create_session(ID3D11Device* device, const xr_panel& panel)
{
    if (!impl_ || impl_->instance == XR_NULL_HANDLE) { return false; }
    impl_->panel = panel;

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next     = &binding;
    sci.systemId = impl_->system;
    XrResult r = xrCreateSession(impl_->instance, &sci, &impl_->session);
    if (XR_FAILED(r)) { impl_->set_error_xr("xrCreateSession", r); return false; }

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace = identity_pose;
    r = xrCreateReferenceSpace(impl_->session, &rsci, &impl_->space);
    if (XR_FAILED(r)) { impl_->set_error_xr("xrCreateReferenceSpace", r); return false; }

    // one pose action (aim ray) and one boolean action (trigger), each bound per-hand
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "strata_ui");
    std::strcpy(asci.localizedActionSetName, "Strata UI");
    r = xrCreateActionSet(impl_->instance, &asci, &impl_->action_set);
    if (XR_FAILED(r)) { impl_->set_error_xr("xrCreateActionSet", r); return false; }

    xrStringToPath(impl_->instance, "/user/hand/left", &impl_->hand_paths[0]);
    xrStringToPath(impl_->instance, "/user/hand/right", &impl_->hand_paths[1]);

    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strcpy(aci.actionName, "aim_pose");
    std::strcpy(aci.localizedActionName, "Aim Pose");
    aci.countSubactionPaths = static_cast<uint32_t>(impl_->hand_paths.size());
    aci.subactionPaths      = impl_->hand_paths.data();
    r = xrCreateAction(impl_->action_set, &aci, &impl_->aim_action);
    if (XR_FAILED(r)) { impl_->set_error_xr("xrCreateAction (aim_pose)", r); return false; }

    XrActionCreateInfo tci{XR_TYPE_ACTION_CREATE_INFO};
    tci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strcpy(tci.actionName, "trigger");
    std::strcpy(tci.localizedActionName, "Trigger");
    tci.countSubactionPaths = static_cast<uint32_t>(impl_->hand_paths.size());
    tci.subactionPaths      = impl_->hand_paths.data();
    r = xrCreateAction(impl_->action_set, &tci, &impl_->trigger_action);
    if (XR_FAILED(r)) { impl_->set_error_xr("xrCreateAction (trigger)", r); return false; }

    // khr/simple_controller is the one interaction profile every conformant runtime supports, at the cost of a
    // generic "select" click rather than each controller's real analog trigger -- fine for a ui pointer, and it
    // means this doesn't need per-headset (touch / index / vive wand) binding tables to work at all.
    XrPath profile{};
    xrStringToPath(impl_->instance, "/interaction_profiles/khr/simple_controller", &profile);
    XrPath aim_left{}, aim_right{}, select_left{}, select_right{};
    xrStringToPath(impl_->instance, "/user/hand/left/input/aim/pose", &aim_left);
    xrStringToPath(impl_->instance, "/user/hand/right/input/aim/pose", &aim_right);
    xrStringToPath(impl_->instance, "/user/hand/left/input/select/click", &select_left);
    xrStringToPath(impl_->instance, "/user/hand/right/input/select/click", &select_right);

    const std::array<XrActionSuggestedBinding, 4> bindings{{
        {impl_->aim_action, aim_left}, {impl_->aim_action, aim_right},
        {impl_->trigger_action, select_left}, {impl_->trigger_action, select_right},
    }};
    XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggest.interactionProfile     = profile;
    suggest.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggest.suggestedBindings      = bindings.data();
    r = xrSuggestInteractionProfileBindings(impl_->instance, &suggest);
    if (XR_FAILED(r)) { impl_->set_error_xr("xrSuggestInteractionProfileBindings", r); return false; }

    for (u32 i = 0; i < max_xr_controllers; ++i) {
        XrActionSpaceCreateInfo spci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spci.action            = impl_->aim_action;
        spci.subactionPath     = impl_->hand_paths[i];
        spci.poseInActionSpace = identity_pose;
        r = xrCreateActionSpace(impl_->session, &spci, &impl_->hand_spaces[i]);
        if (XR_FAILED(r)) { impl_->set_error_xr("xrCreateActionSpace", r); return false; }
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets      = &impl_->action_set;
    r = xrAttachSessionActionSets(impl_->session, &attach);
    if (XR_FAILED(r)) { impl_->set_error_xr("xrAttachSessionActionSets", r); return false; }

    // one rgba8 image: the compositor samples it as a quad layer, no per-eye array or depth buffer needed
    XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swci.format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    swci.sampleCount = 1;
    swci.width      = panel.pixel_width;
    swci.height     = panel.pixel_height;
    swci.faceCount  = 1;
    swci.arraySize  = 1;
    swci.mipCount   = 1;
    r = xrCreateSwapchain(impl_->session, &swci, &impl_->swapchain);
    if (XR_FAILED(r)) { impl_->set_error_xr("xrCreateSwapchain", r); return false; }

    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(impl_->swapchain, 0, &image_count, nullptr);
    impl_->swapchain_images.assign(image_count, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    r = xrEnumerateSwapchainImages(impl_->swapchain, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(impl_->swapchain_images.data()));
    if (XR_FAILED(r)) { impl_->set_error_xr("xrEnumerateSwapchainImages", r); return false; }

    return true;
}

void xr_session::destroy() noexcept { impl_.reset(); }

void xr_session::set_panel(const xr_panel& panel) noexcept
{
    if (!impl_) { return; }
    // pixel_width/height are fixed by the swapchain created in create_session(); only placement can move each frame
    impl_->panel.pose          = panel.pose;
    impl_->panel.width_meters  = panel.width_meters;
    impl_->panel.height_meters = panel.height_meters;
}

const xr_panel& xr_session::panel() const noexcept { return impl_->panel; }

bool xr_session::poll_events()
{
    if (!impl_) { return false; }
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(impl_->instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& sse = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            impl_->session_state = sse.state;
            if (sse.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = impl_->view_config;
                if (XR_SUCCEEDED(xrBeginSession(impl_->session, &bi))) { impl_->running = true; }
            } else if (sse.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(impl_->session);
                impl_->running = false;
            } else if (sse.state == XR_SESSION_STATE_EXITING || sse.state == XR_SESSION_STATE_LOSS_PENDING) {
                return false;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            return false;
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

bool xr_session::session_running() const noexcept { return impl_ && impl_->running; }

bool xr_session::begin_frame(bool& should_render)
{
    should_render = false;
    if (!impl_ || !impl_->running) { return false; }

    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState    fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(impl_->session, &wi, &fs))) { return false; }
    impl_->predicted_display_time = fs.predictedDisplayTime;

    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(impl_->session, &bi))) { return false; }

    should_render              = fs.shouldRender != XR_FALSE;
    impl_->should_render_last  = should_render;
    if (!should_render) { return true; }

    XrActiveActionSet aas{impl_->action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets      = &aas;
    xrSyncActions(impl_->session, &sync);

    for (u32 i = 0; i < max_xr_controllers; ++i) {
        xr_controller& c = impl_->controller_state[i];
        c = {};

        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action        = impl_->trigger_action;
        gi.subactionPath = impl_->hand_paths[i];
        XrActionStateBoolean trigger{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(impl_->session, &gi, &trigger);
        c.trigger_down = trigger.isActive != XR_FALSE && trigger.currentState != XR_FALSE;

        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(impl_->hand_spaces[i], impl_->space, impl_->predicted_display_time, &loc))) {
            constexpr XrSpaceLocationFlags need =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((loc.locationFlags & need) == need) {
                c.tracked = true;
                c.aim     = from_xr(loc.pose);
            }
        }
    }

    return true;
}

ID3D11Texture2D* xr_session::acquire_target()
{
    if (!impl_ || impl_->swapchain == XR_NULL_HANDLE) { return nullptr; }

    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(impl_->swapchain, &ai, &index))) { return nullptr; }

    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(impl_->swapchain, &wi))) { return nullptr; }

    return impl_->swapchain_images[index].texture;
}

void xr_session::end_frame(bool did_render)
{
    if (!impl_) { return; }

    XrCompositionLayerQuad              quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[1]{};
    uint32_t                            layer_count = 0;

    if (did_render && impl_->should_render_last) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(impl_->swapchain, &ri);

        quad.space               = impl_->space;
        quad.eyeVisibility       = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain  = impl_->swapchain;
        quad.subImage.imageRect  = {{0, 0},
                                    {static_cast<int32_t>(impl_->panel.pixel_width), static_cast<int32_t>(impl_->panel.pixel_height)}};
        quad.pose                = to_xr(impl_->panel.pose);
        quad.size                = {impl_->panel.width_meters, impl_->panel.height_meters};
        layers[0]                = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        layer_count               = 1;
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime         = impl_->predicted_display_time;
    ei.environmentBlendMode = impl_->blend_mode;
    ei.layerCount           = layer_count;
    ei.layers               = layers;
    xrEndFrame(impl_->session, &ei);
}

std::span<const xr_controller> xr_session::controllers() const noexcept
{
    return impl_ ? std::span<const xr_controller>{impl_->controller_state} : std::span<const xr_controller>{};
}

const char* xr_session::last_error() const noexcept { return impl_ ? impl_->error : ""; }

// panel-local basis: pose.orientation's local +x is "right" across the panel, +y is "up", and following openxr's
// -z-is-forward convention (same as the aim pose below), local -z is the side the panel is visible from.
// NOTE: this facing convention (which side of the quad the compositor actually draws) is reasoned from the spec,
// not verified against a headset yet -- if the panel renders back-to-front or the ray hit-test feels mirrored,
// flip the sign on `normal` below.
input_state xr_pointer_input(std::span<const xr_controller> controllers, const xr_panel& panel,
                             input_state base) noexcept
{
    const xr_vec3 normal = rotate(panel.pose.orientation, xr_vec3{0.0f, 0.0f, -1.0f});
    const xr_vec3 right  = rotate(panel.pose.orientation, xr_vec3{1.0f, 0.0f, 0.0f});
    const xr_vec3 up     = rotate(panel.pose.orientation, xr_vec3{0.0f, 1.0f, 0.0f});

    int  active = -1;
    vec2 hit_uv{};
    for (std::size_t i = 0; i < controllers.size(); ++i) {
        const xr_controller& c = controllers[i];
        if (!c.tracked) { continue; }

        const xr_vec3 dir   = rotate(c.aim.orientation, xr_vec3{0.0f, 0.0f, -1.0f});
        const f32     denom = dot(dir, normal);
        if (denom > -1e-5f) { continue; } // parallel to, or pointing away from, the panel's front face

        const f32 t = dot(sub(panel.pose.position, c.aim.position), normal) / denom;
        if (t <= 0.0f) { continue; } // panel is behind the controller

        const xr_vec3 hit   = {c.aim.position.x + dir.x * t, c.aim.position.y + dir.y * t, c.aim.position.z + dir.z * t};
        const xr_vec3 local = sub(hit, panel.pose.position);
        const f32 u = dot(local, right) / panel.width_meters + 0.5f;
        const f32 v = 0.5f - dot(local, up) / panel.height_meters;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) { continue; } // hits the plane outside the quad

        const bool prefer = active < 0 ||
                             (c.trigger_down && !controllers[static_cast<std::size_t>(active)].trigger_down);
        if (prefer) {
            active = static_cast<int>(i);
            hit_uv = {u, v};
        }
    }

    if (active >= 0) {
        base.mouse_pos     = {hit_uv.x * static_cast<f32>(panel.pixel_width), hit_uv.y * static_cast<f32>(panel.pixel_height)};
        base.mouse_down[0] = controllers[static_cast<std::size_t>(active)].trigger_down;
    }
    return base;
}

} // namespace strata
