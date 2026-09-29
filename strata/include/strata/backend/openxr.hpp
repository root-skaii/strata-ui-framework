#pragma once

#include "strata/backend/vr_types.hpp"
#include "strata/context.hpp" // input_state
#include "strata/draw_list.hpp"
#include "strata/types.hpp"

#include <memory>
#include <span>

struct ID3D11Device;
struct ID3D11Texture2D;
struct _LUID;
using LUID = _LUID;

namespace strata {

// strata itself stays 2d; these only place the flat ui panel in the headset's tracking volume and let controllers
// be raycast against it. matches openxr's convention (-z forward, +y up). shared with the openvr backend
// (vr_types.hpp) since both describe the same kind of tracked pose.
using xr_vec3 = vr_vec3;
using xr_quat = vr_quat;
using xr_pose = vr_pose;

struct xr_controller {
    bool    tracked{};        // aim pose valid this frame (headset asleep / hand out of view: false)
    xr_pose aim{};             // aim ray origin/orientation, in the session's reference space; -z is the ray direction
    bool    trigger_down{};
};

inline constexpr u32 max_xr_controllers = 2; // left [0], right [1]

// the panel is a composition quad layer, not a 3d mesh: the compositor stereo-renders it, strata renders one 2d
// image same as any other backend. pixel_width/height must match what create_session() sized the swapchain to.
struct xr_panel {
    xr_pose pose{};                    // centre of the quad, in the session's reference space
    f32     width_meters  = 0.6f;
    f32     height_meters = 0.4f;
    u32     pixel_width   = 1280;
    u32     pixel_height  = 853;
};

// owns the openxr instance, session, swapchain and action set. the caller owns the d3d11 device, same division as
// d3d11_renderer -- except here the runtime dictates *which* adapter that device must be created on, so this is a
// two-step handshake: create_instance() first (needs no device), read adapter_luid(), create a device on that
// adapter, then create_session(device, ...).
class xr_session {
public:
    xr_session() noexcept;
    ~xr_session();

    xr_session(const xr_session&)            = delete;
    xr_session& operator=(const xr_session&) = delete;
    xr_session(xr_session&&) noexcept;
    xr_session& operator=(xr_session&&) noexcept;

    // instance + system. false if no openxr runtime is installed / active, or no hmd is found (see last_error()).
    [[nodiscard]] bool create_instance(const char* app_name = "strata");
    // valid only after create_instance() succeeds: the adapter the runtime requires the app's d3d11 device on.
    [[nodiscard]] const LUID& adapter_luid() const noexcept;

    // session + swapchain, on a device already created on adapter_luid(). panel.pixel_width/height size the
    // swapchain; panel.pose/width_meters/height_meters can still change every frame via set_panel().
    [[nodiscard]] bool create_session(ID3D11Device* device, const xr_panel& panel);
    void destroy() noexcept;

    void set_panel(const xr_panel& panel) noexcept;
    [[nodiscard]] const xr_panel& panel() const noexcept;

    // pumps xrPollEvent. false once the runtime wants the app to exit (quit in the headset, hmd unplugged).
    [[nodiscard]] bool poll_events();
    // true once the session is visible/focused and frames should be driven; call after poll_events(), every iteration
    [[nodiscard]] bool session_running() const noexcept;

    // xrWaitFrame + xrBeginFrame. should_render false: the runtime asked the app not to draw (not visible / not
    // focused) but still expects a matching end_frame(false) -- do not skip the call.
    [[nodiscard]] bool begin_frame(bool& should_render);
    // this frame's render target; valid only between begin_frame() (should_render true) and end_frame().
    [[nodiscard]] ID3D11Texture2D* acquire_target();
    // xrEndFrame; submits the quad layer built from panel() when did_render is true, an empty frame otherwise.
    void end_frame(bool did_render);

    // controller state as of the last begin_frame().
    [[nodiscard]] std::span<const xr_controller> controllers() const noexcept;

    [[nodiscard]] const char* last_error() const noexcept;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// raycasts each tracked controller against the panel's plane and fills mouse_pos/mouse_down[0] in `base` for
// whichever one is the active pointer this frame. arbitration: a controller already holding the trigger keeps the
// cursor even if the other one's ray also hits the panel this frame; otherwise the first hit wins. a controller
// that doesn't hit the panel never moves the cursor, even while tracked.
[[nodiscard]] input_state xr_pointer_input(std::span<const xr_controller> controllers, const xr_panel& panel,
                                           input_state base) noexcept;

} // namespace strata
