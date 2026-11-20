#include "limits.hpp"

#ifndef STRATA_NO_DIAGNOSTICS

#include "context_impl.hpp"
#include "context_helpers.hpp"

#include <algorithm>

namespace strata::internal {

void diag_marks::add(std::string_view message, const rect& box) noexcept
{
    for (u32 i = 0; i < count; ++i) { // (a limit hit by forty windows is one mark)
        const entry& e = items[i];
        if (std::string_view{e.text.data(), e.len} == message && e.box.min == box.min && e.box.max == box.max) { return; }
    }
    if (count == max_entries) { return; }
    entry& e = items[count++];
    e.box    = box;
    e.len    = static_cast<u8>(std::min(message.size(), e.text.size()));
    std::copy_n(message.data(), e.len, e.text.data());
}

void diag_marks_draw(context& ui)
{
    auto& m = context_access::of(ui);
    diag_marks& marks = m.diag_marks_;
    if (marks.count == 0) { return; }

    const font_id f  = 0;
    const f32 lh     = m.font_.line_height(f);
    const f32 pad    = 8.0f;
    const color bad  = m.style_.error;

    const u32 previous_owner = m.run_owner_;
    switch_run(ui, run_foreground);
    m.dl_.push_clip_absolute({{0.0f, 0.0f}, m.display_});

    // a panel of message lines, its top-left corner at `at` (kept on screen)
    const auto panel = [&](vec2 at, const std::array<std::string_view, diag_marks::max_entries>& lines, u32 n) {
        f32 w = 0.0f;
        for (u32 i = 0; i < n; ++i) { w = std::max(w, m.font_.measure(f, lines[i]).x); }
        const vec2 size = {w + 2.0f * pad, lh * static_cast<f32>(n) + 2.0f * pad};
        at.x = std::max(0.0f, std::min(at.x, m.display_.x - size.x));
        at.y = std::max(0.0f, std::min(at.y, m.display_.y - size.y));
        const rect r = rect::from_size(at, size);
        m.dl_.rect_filled(r, color{m.style_.window_bg.r, m.style_.window_bg.g, m.style_.window_bg.b, 255}, 4.0f);
        m.dl_.rect_outline(r, bad, 4.0f, 1.5f);
        for (u32 i = 0; i < n; ++i) { m.dl_.text({at.x + pad, at.y + pad + lh * static_cast<f32>(i)}, bad, lines[i], f); }
    };

    std::array<std::string_view, diag_marks::max_entries> hovered, loose;
    u32 n_hovered = 0, n_loose = 0;
    for (u32 i = 0; i < marks.count; ++i) {
        const diag_marks::entry& e = marks.items[i];
        const std::string_view text{e.text.data(), e.len};
        if (e.box.empty()) {
            loose[n_loose++] = text;
            continue;
        }
        m.dl_.rect_outline(e.box.expanded(1.0f), bad, 0.0f, 2.0f);
        if (e.box.contains(m.input_.mouse_)) { hovered[n_hovered++] = text; }
    }
    if (n_loose != 0)   { panel({8.0f, 8.0f}, loose, n_loose); }
    if (n_hovered != 0) { panel({m.input_.mouse_.x + 16.0f, m.input_.mouse_.y + 18.0f}, hovered, n_hovered); }

    m.dl_.pop_clip();
    switch_run(ui, previous_owner);
    marks.count = 0;
}

} // namespace strata::internal

#endif
