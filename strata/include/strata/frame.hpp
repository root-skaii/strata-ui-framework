#pragma once

// the per-frame steps every host repeats, written once over any platform_backend:
//
//     while (running) {
//         if (strata::run_frame(ui, platform, [&](strata::context& ui) { /* build the ui */ })) {
//             renderer.render(ui.render_data());   // something changed; the render call is the only api-specific part
//             present();
//         }
//     }
//
// hosts that need to adjust the input or act between the steps (an overlay remapping the mouse, a vr panel with
// pointer rays) call begin_frame / end_frame themselves, or keep their own loop.

#include "strata/backend/backend.hpp"
#include "strata/context.hpp"

#include <utility>

namespace strata {

// platform input -> ui.begin_frame
template <platform_backend P>
void begin_frame(context& ui, P& platform)
{
    ui.begin_frame(platform.new_frame());
}

// ui.end_frame, then what the platform needs from the finished frame: IME placement and the pointer shape
template <platform_backend P>
void end_frame(context& ui, P& platform)
{
    ui.end_frame();
    platform.set_ime(ui.ime_wanted(), ui.ime_position(), ui.ime_line_height());
    platform.set_cursor(ui.cursor());
}

// one whole frame. true when the geometry changed (or `allow_idle` is false): the host should render and present
template <platform_backend P, class Build>
    requires std::invocable<Build&, context&>
[[nodiscard]] bool run_frame(context& ui, P& platform, Build&& build, bool allow_idle = true)
{
    begin_frame(ui, platform);
    build(ui);
    end_frame(ui, platform);
    return !allow_idle || !ui.frame_unchanged();
}

} // namespace strata
