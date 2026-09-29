#pragma once

#include "strata/backend/vr_types.hpp"
#include "strata/context.hpp" // input_state
#include "strata/types.hpp"

#include <memory>
#include <optional>
#include <span>

struct ID3D11Texture2D;

namespace strata {

struct vr_controller {
    bool    tracked{};      // device tracked and the overlay intersection test ran this frame
    vr_pose aim{};           // controller pose: steamvr input system's per-model "tip" pose when available (see
                              // vr_overlay::input_system_ready()), otherwise the device's raw tracking pose
    bool    aim_from_tip{}; // true if `aim` came from the input system's tip pose, false if it's the raw fallback
    bool    trigger_down{};
    bool    hit{};           // the controller's forward ray currently hits the overlay (see vr_pointer_input)
    vec2    hit_uv{};        // where, in 0..1 overlay space, valid only while `hit`
    vec2    joystick{};      // legacy axis 0 (-1..1 each): the primary stick/trackpad, whichever this model has
};

inline constexpr u32 max_vr_controllers = 2; // left [0], right [1]

// the panel is an IVROverlay quad: steamvr composites and stereo-renders it, strata renders one 2d image same as
// any other backend. height follows the aspect ratio of whatever texture submit_texture() is given.
struct vr_panel {
    // centre of the quad. a world pose in the tracking-universe origin (see vr_overlay::create) when head_locked
    // is false (the default) -- the panel stays put in the room. an offset in the hmd's own local space when
    // head_locked is true (e.g. {0,0,-1} sits a metre ahead of wherever your head points): steamvr re-derives its
    // absolute pose from the hmd every frame on its own, so it rides along with near-zero added latency, unlike
    // resubmitting an absolute pose from this app's own loop-paced reads of the hmd.
    vr_pose pose{};
    f32     width_meters = 0.6f;
    bool    head_locked  = false;
};

// which vr api (if any) is already active in this process -- checks loaded modules, not installed runtimes: a
// game that has not entered vr yet has loaded neither, even with steamvr / an openxr runtime installed and
// running. poll this periodically rather than once, since a game may load its xr backend well after this dll does.
enum class vr_kind : u8 { none, openxr, openvr };
[[nodiscard]] vr_kind detect_vr() noexcept;

// wraps IVROverlay: a compositor-drawn quad, positioned in the tracking volume, fed a d3d11 texture every frame.
// unlike an openxr xr_session, this never renders anything itself and never touches a game's own present/submit
// calls -- it is steamvr's own mechanism for a second application to draw on top (the same one the SteamVR
// dashboard and desktop overlay windows use), so there is nothing to hook and nothing needs to be created "on"
// the game's own vr session.
class vr_overlay {
public:
    vr_overlay() noexcept;
    ~vr_overlay();

    vr_overlay(const vr_overlay&)            = delete;
    vr_overlay& operator=(const vr_overlay&) = delete;
    vr_overlay(vr_overlay&&) noexcept;
    vr_overlay& operator=(vr_overlay&&) noexcept;

    // if openvr is already initialised in this process (a game that called VR_Init), reuses it; otherwise calls
    // VR_Init(VRApplication_Overlay) itself -- needs SteamVR running, but not a headset rendering session, since
    // an overlay-type application never submits eye frames. false if neither is possible (see last_error()).
    [[nodiscard]] bool create(const char* key, const char* friendly_name, const vr_panel& panel);
    void destroy() noexcept;

    // true if this call made the VR_Init call (and so is responsible for VR_Shutdown on destroy) rather than
    // reusing a host game's; mainly diagnostic.
    [[nodiscard]] bool owns_init() const noexcept;

    void set_panel(const vr_panel& panel) noexcept;
    [[nodiscard]] const vr_panel& panel() const noexcept;

    void show() noexcept;
    void hide() noexcept;
    [[nodiscard]] bool visible() const noexcept;

    // uploads this frame's render target; call straight after rendering into `texture` (its own dimensions become
    // the overlay's pixel size and aspect ratio -- panel::width_meters is the only size steamvr is told directly).
    void submit_texture(ID3D11Texture2D* texture) noexcept;

    // refreshes controller poses and runs IVROverlay::ComputeOverlayIntersection for each -- call once per frame
    // before reading controllers() or calling vr_pointer_input().
    void update_controllers() noexcept;
    [[nodiscard]] std::span<const vr_controller> controllers() const noexcept;

    // true if create() stood up the SteamVR Input System action manifest, so controllers() should be getting each
    // hand's calibrated tip pose rather than its raw, harder-to-aim device pose.
    [[nodiscard]] bool input_system_ready() const noexcept;

    [[nodiscard]] const char* last_error() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// fills mouse_pos/mouse_down[0] in `base` from whichever controller currently hits the overlay (the raycast
// itself is openvr's own ComputeOverlayIntersection, run by update_controllers()); `base.display_size` must
// already be set to the submitted texture's pixel size, to turn the intersection's 0..1 UV into pixels.
// arbitration matches xr_pointer_input: a controller already holding the trigger keeps the cursor over one that
// only just started hitting the panel; otherwise the first hit wins.
[[nodiscard]] input_state vr_pointer_input(std::span<const vr_controller> controllers, input_state base) noexcept;

// a pose `distance_m` in front of the hmd's *current* position, facing back at the user, level (pitch/roll
// flattened out). unlike openxr's LOCAL space (anchored at session start), TrackingUniverseStanding's origin is
// the room's fixed chaperone origin, so a panel needs placing relative to the hmd explicitly. call after
// vr_overlay::create() -- once for a panel that stays put, or every frame for a head-locked one. a single query,
// cheap enough per-frame; nullopt if the hmd isn't tracked this instant (callers should leave the panel as is
// rather than apply a default pose: world origin, identity orientation).
[[nodiscard]] std::optional<vr_pose> vr_pose_in_front_of_hmd(float distance_m = 1.2f) noexcept;

} // namespace strata
