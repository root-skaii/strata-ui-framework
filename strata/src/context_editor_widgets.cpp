// Engine & creative-tool widgets: timeline / sequencer, Bezier curve editor, and two-column property inspector grid.

#include "context_helpers.hpp"
#include "context_impl.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace strata {

using namespace internal;

namespace {

[[nodiscard]] f32 snap_val(f32 v, f32 step) noexcept
{
    return step > 0.0f ? std::round(v / step) * step : v;
}

[[nodiscard]] vec2 cubic_eval(vec2 p0, vec2 p1, vec2 p2, vec2 p3, f32 t) noexcept
{
    const f32 u = 1.0f - t;
    const f32 uu = u * u, tt = t * t;
    return p0 * (uu * u) + p1 * (3.0f * uu * t) + p2 * (3.0f * u * tt) + p3 * (tt * t);
}

} // namespace

f32 eval_curve(std::span<const curve_point> points, f32 x) noexcept
{
    if (points.empty()) { return 0.0f; }
    if (points.size() == 1 || x <= points.front().x) { return points.front().y; }
    if (x >= points.back().x) { return points.back().y; }

    std::size_t seg = 0;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        if (x >= points[i].x && x <= points[i + 1].x) { seg = i; break; }
    }
    const curve_point& a = points[seg];
    const curve_point& b = points[seg + 1];
    const vec2 p0{a.x, a.y};
    const vec2 p3{b.x, b.y};
    const vec2 p1 = p0 + a.out_tangent;
    const vec2 p2 = p3 + b.in_tangent;

    // 10 iterations of binary search on t in [0, 1] to find x(t) = x
    f32 lo = 0.0f, hi = 1.0f;
    for (int it = 0; it < 10; ++it) {
        const f32 mid = 0.5f * (lo + hi);
        if (cubic_eval(p0, p1, p2, p3, mid).x < x) { lo = mid; } else { hi = mid; }
    }
    return cubic_eval(p0, p1, p2, p3, 0.5f * (lo + hi)).y;
}

// ---- timeline / sequencer ------------------------------------------------------------------------------------------

bool context::begin_timeline(std::string_view label, timeline_state& state, vec2 size)
{
    if (m_->cur_ == nullptr || m_->timeline_.active) { return false; }

    const font_id f = current_font();
    const id key    = widget_id(*this, label);
    const f32 w     = size.x > 0.0f ? size.x : content_width();
    const f32 h     = size.y > 0.0f ? size.y : 220.0f;
    const rect box  = layout_place(*this, {w, h});

    if (state.end <= state.start) { state.end = state.start + 1.0f; }
    if (state.view_end <= state.view_start) {
        state.view_start = state.start;
        state.view_end   = state.end;
    }
    state.time = std::clamp(state.time, state.start, state.end);

    const f32 ruler_h = m_->font_.line_height(f) + 8.0f;
    const f32 lw      = std::clamp(state.label_width, 64.0f, std::max(64.0f, w * 0.5f));
    const rect ruler  = {{box.min.x + lw, box.min.y}, {box.max.x, box.min.y + ruler_h}};
    const rect tracks = {{box.min.x, box.min.y + ruler_h}, box.max};

    // background & frame
    shape_style bg;
    bg.radius       = radii(m_->style_.rounding * 0.8f);
    bg.fill_top     = color{0, 0, 0, 56};
    bg.fill_bottom  = bg.fill_top;
    bg.border       = m_->style_.widget_border;
    bg.border_width = m_->style_.border_width;
    m_->dl_.shape(box, bg);
    m_->dl_.rect_filled({{box.min.x, box.min.y}, {box.max.x, box.min.y + ruler_h}}, m_->style_.title_bg.scaled_alpha(0.75f));
    m_->dl_.rect_filled({{box.min.x + lw, box.min.y}, {box.min.x + lw + 1.0f, box.max.y}}, m_->style_.widget_border);

    const std::string_view shown = visible_label(label);
    if (!shown.empty()) {
        m_->dl_.text({box.min.x + 8.0f, box.min.y + 4.0f}, m_->style_.text_dim, shown, f);
    }

    // zoom (wheel over timeline) & pan (middle drag / Alt+drag)
    const f32 view_span = std::max(state.view_end - state.view_start, 1.0e-3f);
    const f32 rw        = std::max(ruler.width(), 1.0f);
    const auto x_of = [&](f32 t) { return ruler.min.x + (t - state.view_start) / view_span * rw; };
    const auto t_of = [&](f32 x) { return state.view_start + (x - ruler.min.x) / rw * view_span; };

    if (pointer_over(*this, box)) {
        if (m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_) {
            m_->wheel_consumed_ = true;
            const f32 pivot = std::clamp(t_of(m_->input_.mouse_.x), state.view_start, state.view_end);
            const f32 k     = std::pow(0.85f, m_->input_.wheel_);
            const f32 total = state.end - state.start;
            const f32 ns    = std::clamp(view_span * k, std::max(total * 0.02f, 1.0f), total);
            const f32 rel   = (pivot - state.view_start) / view_span;
            state.view_start = std::clamp(pivot - rel * ns, state.start, state.end - ns);
            state.view_end   = state.view_start + ns;
        }
        if ((m_->input_.mouse_middle_down_ || (m_->input_.mod_alt_ && m_->input_.mouse_down_)) &&
            m_->input_.mouse_delta_.x != 0.0f) {
            const f32 dt = -m_->input_.mouse_delta_.x / rw * view_span;
            const f32 lo = std::clamp(state.view_start + dt, state.start, state.end - view_span);
            state.view_start = lo;
            state.view_end   = lo + view_span;
        }
    }

    // ruler scrubbing
    const interaction ri = interact(*this, hash_id("##ruler", key), ruler);
    if (ri.held || (ri.hovered && m_->input_.mouse_pressed_)) {
        state.time = std::clamp(snap_val(t_of(m_->input_.mouse_.x), state.snap), state.start, state.end);
    }

    // ruler ticks
    m_->dl_.push_clip(ruler);
    const f32 approx_step = view_span / std::max(rw / 64.0f, 1.0f);
    const f32 mag         = std::pow(10.0f, std::floor(std::log10(std::max(approx_step, 1.0e-4f))));
    const f32 frac        = approx_step / mag;
    const f32 tick_step   = (frac <= 1.5f ? 1.0f : frac <= 3.0f ? 2.0f : frac <= 7.0f ? 5.0f : 10.0f) * mag;
    const long long k0    = static_cast<long long>(std::floor(state.view_start / tick_step));
    for (int it = 0; it < 256; ++it) {
        const f32 t = static_cast<f32>(k0 + it) * tick_step;
        if (t > state.view_end + tick_step * 0.01f) { break; }
        const f32 x = std::round(x_of(t));
        m_->dl_.rect_filled({{x, ruler.max.y - 6.0f}, {x + 1.0f, ruler.max.y}}, m_->style_.text_dim.scaled_alpha(0.6f));
        char buf[24];
        std::snprintf(buf, sizeof buf, tick_step >= 1.0f ? "%.0f" : "%.2f", static_cast<double>(t));
        m_->dl_.text({x + 3.0f, ruler.min.y + 3.0f}, m_->style_.text_dim, buf, f);
    }
    m_->dl_.pop_clip();

    m_->timeline_ = {true, key, &state, box, ruler, tracks, tracks.min.y, 0};
    m_->dl_.push_clip(tracks);
    return true;
}

timeline_track_result context::timeline_track(std::string_view name, std::span<timeline_clip> clips,
                                              std::span<timeline_keyframe> keys)
{
    timeline_track_result res;
    if (!m_->timeline_.active || m_->timeline_.state == nullptr) { return res; }
    auto& tl = m_->timeline_;
    timeline_state& st = *tl.state;

    const font_id f  = current_font();
    const f32 row_h  = std::max(frame_height() + 4.0f, 26.0f);
    const f32 y0     = tl.cursor_y;
    const f32 y1     = y0 + row_h;
    tl.cursor_y      = y1;
    const u32 tidx   = tl.track_idx++;
    if (y0 > tl.tracks.max.y) { return res; }

    const rect row_r{{tl.tracks.min.x, y0}, {tl.tracks.max.x, y1}};
    const rect lbl_r{{tl.tracks.min.x, y0}, {tl.ruler.min.x, y1}};
    const rect lane_r{{tl.ruler.min.x, y0}, {tl.tracks.max.x, y1}};

    if ((tidx & 1u) != 0) {
        m_->dl_.rect_filled(row_r, color{255, 255, 255, 10});
    }
    m_->dl_.rect_filled({{row_r.min.x, y1 - 1.0f}, {row_r.max.x, y1}}, m_->style_.widget_border.scaled_alpha(0.45f));
    m_->dl_.push_clip(lbl_r);
    m_->dl_.text({lbl_r.min.x + 8.0f, y0 + (row_h - m_->font_.line_height(f)) * 0.5f}, m_->style_.text, name, f);
    m_->dl_.pop_clip();

    const f32 span = std::max(st.view_end - st.view_start, 1.0e-3f);
    const f32 rw   = std::max(lane_r.width(), 1.0f);
    const auto x_of = [&](f32 t) { return lane_r.min.x + (t - st.view_start) / span * rw; };
    const auto t_of = [&](f32 x) { return st.view_start + (x - lane_r.min.x) / rw * span; };

    if (!m_->input_.mouse_down_) { m_->timeline_drag_id_ = 0; }

    m_->dl_.push_clip(lane_r);
    const id track_key = hash_id(name, hash_id({reinterpret_cast<const char*>(&tidx), sizeof(tidx)}, tl.key));

    // Hit-test keyframes before clips so diamonds sitting over a clip remain directly grabbable,
    // while still drawing clips first and diamonds on top.
    const f32 ky = 0.5f * (y0 + y1);
    std::array<interaction, 64> key_in_stack{};
    std::vector<interaction>    key_in_heap;
    interaction*                key_ins = key_in_stack.data();
    if (keys.size() > key_in_stack.size()) {
        key_in_heap.resize(keys.size());
        key_ins = key_in_heap.data();
    }
    for (std::size_t ki = 0; ki < keys.size(); ++ki) {
        timeline_keyframe& k = keys[ki];
        const id kk = hash_id("##K", hash_id({reinterpret_cast<const char*>(&ki), sizeof(ki)}, track_key));
        const f32 kx = x_of(k.time);
        if ((kx < lane_r.min.x - 8.0f || kx > lane_r.max.x + 8.0f) && m_->active_ != kk) { continue; }
        const f32 r = 5.0f;
        const rect kr{{kx - r - 2.0f, ky - r - 2.0f}, {kx + r + 2.0f, ky + r + 2.0f}};
        const interaction in_k = interact(*this, kk, kr);
        key_ins[ki] = in_k;
        if (in_k.held) {
            if (m_->timeline_drag_id_ != kk) {
                m_->timeline_drag_id_   = kk;
                m_->timeline_drag_grab_ = t_of(m_->input_.mouse_.x) - k.time;
            }
            const f32 target = t_of(m_->input_.mouse_.x) - m_->timeline_drag_grab_;
            const f32 next   = std::clamp(snap_val(target, st.snap), st.start, st.end);
            if (next != k.time) { k.time = next; res.changed_key = static_cast<int>(ki); }
        }
        if (in_k.pressed || in_k.held) { res.selected_key = static_cast<int>(ki); }
    }

    // clips: disjoint left/right trim edges + draggable interior body, tracked from absolute pointer position
    // so slow drags accumulate cleanly across snap boundaries.
    for (std::size_t ci = 0; ci < clips.size(); ++ci) {
        timeline_clip& c = clips[ci];
        const id ck   = hash_id({reinterpret_cast<const char*>(&ci), sizeof(ci)}, track_key);
        const id lk_l = hash_id("##L", ck);
        const id lk_r = hash_id("##R", ck);
        const f32 cx0 = x_of(c.start);
        const f32 cx1 = x_of(c.end);
        const bool any_active = (m_->active_ == ck || m_->active_ == lk_l || m_->active_ == lk_r);
        if ((cx1 < lane_r.min.x || cx0 > lane_r.max.x) && !any_active) { continue; }

        const rect cr_hit{{cx0, y0 + 3.0f}, {std::max(cx1, cx0 + 8.0f), y1 - 3.0f}};
        const f32 edge_w = std::clamp(std::floor(cr_hit.width() * 0.22f), 4.0f, 8.0f);
        const rect lr{{cr_hit.min.x, cr_hit.min.y}, {cr_hit.min.x + edge_w, cr_hit.max.y}};
        const rect rr{{cr_hit.max.x - edge_w, cr_hit.min.y}, {cr_hit.max.x, cr_hit.max.y}};
        const rect br{{cr_hit.min.x + edge_w, cr_hit.min.y}, {std::max(cr_hit.max.x - edge_w, cr_hit.min.x + edge_w), cr_hit.max.y}};

        const interaction in_l = interact(*this, lk_l, lr);
        const interaction in_r = interact(*this, lk_r, rr);
        const interaction in_b = interact(*this, ck, br);

        if (in_l.hovered || in_l.held || in_r.hovered || in_r.held) { m_->cursor_ = cursor_kind::resize_ew; }
        const f32 min_dur = std::max(st.snap, 0.1f);
        if (in_l.held) {
            if (m_->timeline_drag_id_ != lk_l) {
                m_->timeline_drag_id_   = lk_l;
                m_->timeline_drag_grab_ = t_of(m_->input_.mouse_.x) - c.start;
            }
            const f32 target = t_of(m_->input_.mouse_.x) - m_->timeline_drag_grab_;
            const f32 next   = std::clamp(snap_val(target, st.snap), st.start, c.end - min_dur);
            if (next != c.start) { c.start = next; res.changed_clip = static_cast<int>(ci); }
        } else if (in_r.held) {
            if (m_->timeline_drag_id_ != lk_r) {
                m_->timeline_drag_id_   = lk_r;
                m_->timeline_drag_grab_ = t_of(m_->input_.mouse_.x) - c.end;
            }
            const f32 target = t_of(m_->input_.mouse_.x) - m_->timeline_drag_grab_;
            const f32 next   = std::clamp(snap_val(target, st.snap), c.start + min_dur, st.end);
            if (next != c.end) { c.end = next; res.changed_clip = static_cast<int>(ci); }
        } else if (in_b.held) {
            if (m_->timeline_drag_id_ != ck) {
                m_->timeline_drag_id_   = ck;
                m_->timeline_drag_grab_ = t_of(m_->input_.mouse_.x) - c.start;
            }
            const f32 dur    = c.end - c.start;
            const f32 target = t_of(m_->input_.mouse_.x) - m_->timeline_drag_grab_;
            const f32 next   = std::clamp(snap_val(target, st.snap), st.start, st.end - dur);
            if (next != c.start) {
                c.start = next;
                c.end   = next + dur;
                res.changed_clip = static_cast<int>(ci);
            }
        }
        if (in_b.pressed || in_b.held || in_l.held || in_r.held) { res.selected_clip = static_cast<int>(ci); }

        // Recompute rect from updated start/end so rendering tracks the pointer with zero 1-frame lag.
        const f32 dx0 = x_of(c.start);
        const f32 dx1 = x_of(c.end);
        const rect cr{{dx0, y0 + 3.0f}, {std::max(dx1, dx0 + 8.0f), y1 - 3.0f}};
        const color base = c.col.a != 0 ? c.col : m_->style_.accent;
        const bool  lit  = in_b.hovered || in_b.held || in_l.hovered || in_l.held || in_r.hovered || in_r.held;
        const color fill = lit ? base.scaled_alpha(0.92f) : base.scaled_alpha(0.75f);
        m_->dl_.rect_filled(cr, fill, 3.0f);
        m_->dl_.rect_outline(cr, base, 3.0f, 1.0f);
        if (in_l.hovered || in_l.held) {
            m_->dl_.rect_filled({{cr.min.x + 1.0f, cr.min.y + 3.0f}, {cr.min.x + 3.5f, cr.max.y - 3.0f}}, color{255, 255, 255, 210}, 1.0f);
        }
        if (in_r.hovered || in_r.held) {
            m_->dl_.rect_filled({{cr.max.x - 3.5f, cr.min.y + 3.0f}, {cr.max.x - 1.0f, cr.max.y - 3.0f}}, color{255, 255, 255, 210}, 1.0f);
        }
        if (!c.label.empty() && cr.width() > 20.0f) {
            m_->dl_.push_clip(cr);
            m_->dl_.text({cr.min.x + 6.0f, cr.min.y + (cr.height() - m_->font_.line_height(f)) * 0.5f},
                         color{255, 255, 255, 240}, c.label, f);
            m_->dl_.pop_clip();
        }
    }

    // draw keyframe diamonds on top of clips
    for (std::size_t ki = 0; ki < keys.size(); ++ki) {
        const timeline_keyframe& k = keys[ki];
        const f32 kx = x_of(k.time);
        if (kx < lane_r.min.x - 8.0f || kx > lane_r.max.x + 8.0f) { continue; }
        const f32 r = 5.0f;
        const interaction in_k = key_ins[ki];
        const color kc = (in_k.hovered || in_k.held) ? m_->style_.accent_hover
                                                     : (k.col.a != 0 ? k.col : m_->style_.accent);
        const vec2 top{kx, ky - r}, right{kx + r, ky}, bot{kx, ky + r}, left{kx - r, ky};
        m_->dl_.triangle_filled(top, right, bot, kc);
        m_->dl_.triangle_filled(top, bot, left, kc);
    }

    m_->dl_.pop_clip();
    return res;
}

void context::end_timeline()
{
    if (!m_->timeline_.active) { return; }
    const auto tl = m_->timeline_;
    m_->dl_.pop_clip(); // tracks clip

    // draw playhead across ruler and all tracks
    if (tl.state != nullptr) {
        const f32 span = std::max(tl.state->view_end - tl.state->view_start, 1.0e-3f);
        const f32 px   = tl.ruler.min.x + (tl.state->time - tl.state->view_start) / span * std::max(tl.ruler.width(), 1.0f);
        if (px >= tl.ruler.min.x - 1.0f && px <= tl.ruler.max.x + 1.0f) {
            const color pc = m_->style_.accent;
            m_->dl_.rect_filled({{std::round(px), tl.ruler.min.y}, {std::round(px) + 1.5f, tl.outer.max.y}}, pc);
            m_->dl_.triangle_filled({px - 5.0f, tl.ruler.min.y}, {px + 5.0f, tl.ruler.min.y}, {px, tl.ruler.min.y + 8.0f}, pc);
        }
    }
    m_->timeline_ = {};
}

// ---- Bezier curve editor -------------------------------------------------------------------------------------------

bool context::curve_editor(std::string_view label, std::vector<curve_point>& points, vec2 size, rect domain)
{
    if (m_->cur_ == nullptr) { return false; }

    const font_id f = current_font();
    const id key    = widget_id(*this, label);
    const f32 w     = size.x > 0.0f ? size.x : content_width();
    const f32 h     = size.y > 0.0f ? size.y : 180.0f;
    const rect box  = layout_place(*this, {w, h});
    const rect inner = box.expanded(-10.0f);
    if (inner.width() < 16.0f || inner.height() < 16.0f) { return false; }

    const f32 dx = std::max(domain.max.x - domain.min.x, 1.0e-4f);
    const f32 dy = std::max(domain.max.y - domain.min.y, 1.0e-4f);
    const auto to_screen = [&](vec2 p) -> vec2 {
        return {inner.min.x + (p.x - domain.min.x) / dx * inner.width(),
                inner.max.y - (p.y - domain.min.y) / dy * inner.height()};
    };
    const auto to_domain = [&](vec2 s) -> vec2 {
        return {domain.min.x + (s.x - inner.min.x) / inner.width() * dx,
                domain.min.y + (inner.max.y - s.y) / inner.height() * dy};
    };

    shape_style bg;
    bg.radius       = radii(m_->style_.rounding * 0.8f);
    bg.fill_top     = color{0, 0, 0, 60};
    bg.fill_bottom  = bg.fill_top;
    bg.border       = m_->style_.widget_border;
    bg.border_width = m_->style_.border_width;
    m_->dl_.shape(box, bg);

    // 4x4 subtle grid
    for (int i = 0; i <= 4; ++i) {
        const f32 t = static_cast<f32>(i) * 0.25f;
        const f32 gx = std::round(inner.min.x + t * inner.width());
        const f32 gy = std::round(inner.min.y + t * inner.height());
        m_->dl_.rect_filled({{gx, inner.min.y}, {gx + 1.0f, inner.max.y}}, color{255, 255, 255, 18});
        m_->dl_.rect_filled({{inner.min.x, gy}, {inner.max.x, gy + 1.0f}}, color{255, 255, 255, 18});
    }
    const std::string_view shown = visible_label(label);
    if (!shown.empty()) {
        m_->dl_.text({box.min.x + 8.0f, box.min.y + 4.0f}, m_->style_.text_dim, shown, f);
    }

    bool changed = false;
    bool hit_any_handle = false;

    // interact with control points and tangent handles first so the curve and dots render with zero 1-frame lag
    for (std::size_t i = 0; i < points.size(); ++i) {
        curve_point& pt = points[i];
        const id pk     = hash_id({reinterpret_cast<const char*>(&i), sizeof(i)}, key);

        const auto handle_tangent = [&](vec2& tang, vec2& other, const char* tag) {
            const vec2 tp = to_screen({pt.x + tang.x, pt.y + tang.y});
            const rect hr{{tp.x - 5.0f, tp.y - 5.0f}, {tp.x + 5.0f, tp.y + 5.0f}};
            const interaction hi = interact(*this, hash_id(tag, pk), hr);
            if (hi.hovered || hi.held) { hit_any_handle = true; }
            if (hi.held && (m_->input_.mouse_delta_.x != 0.0f || m_->input_.mouse_delta_.y != 0.0f)) {
                const vec2 dp = to_domain(m_->input_.mouse_) - vec2{pt.x, pt.y};
                tang = dp;
                if (m_->input_.mod_shift_) { pt.broken = true; }
                if (!pt.broken) { other = -tang; }
                changed = true;
            }
        };
        if (i > 0)                  { handle_tangent(pt.in_tangent, pt.out_tangent, "##tin"); }
        if (i + 1 < points.size())  { handle_tangent(pt.out_tangent, pt.in_tangent, "##tout"); }

        const vec2 sp   = to_screen({pt.x, pt.y});
        const rect pr{{sp.x - 6.0f, sp.y - 6.0f}, {sp.x + 6.0f, sp.y + 6.0f}};
        const interaction pi = interact(*this, pk, pr);
        if (pi.hovered || pi.held) { hit_any_handle = true; }
        if (pi.hovered && m_->input_.mouse_right_pressed_ && points.size() > 2) {
            points.erase(points.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            break;
        }
        if (pi.held && (m_->input_.mouse_delta_.x != 0.0f || m_->input_.mouse_delta_.y != 0.0f)) {
            const vec2 np = to_domain(m_->input_.mouse_);
            const f32 lo_x = i > 0 ? points[i - 1].x + dx * 0.01f : domain.min.x;
            const f32 hi_x = i + 1 < points.size() ? points[i + 1].x - dx * 0.01f : domain.max.x;
            pt.x = std::clamp(np.x, lo_x, hi_x);
            pt.y = std::clamp(np.y, domain.min.y, domain.max.y);
            changed = true;
        }
    }

    // double-click background to insert a new point
    const interaction bg_in = interact(*this, hash_id("##bg", key), inner);
    if (!hit_any_handle && bg_in.hovered && item_double_clicked()) {
        const vec2 np = to_domain(m_->input_.mouse_);
        curve_point cp;
        cp.x = std::clamp(np.x, domain.min.x, domain.max.x);
        cp.y = std::clamp(np.y, domain.min.y, domain.max.y);
        cp.in_tangent  = {-dx * 0.08f, 0.0f};
        cp.out_tangent = {dx * 0.08f, 0.0f};
        auto pos = std::lower_bound(points.begin(), points.end(), cp.x, [](const curve_point& a, f32 x) { return a.x < x; });
        points.insert(pos, cp);
        changed = true;
    }

    // draw evaluated curve polyline, then tangent arms and control points on top
    if (points.size() >= 2) {
        m_->dl_.push_clip(inner);
        std::array<vec2, 65> samples{};
        for (std::size_t s = 0; s < samples.size(); ++s) {
            const f32 t = static_cast<f32>(s) / static_cast<f32>(samples.size() - 1);
            const f32 x = domain.min.x + t * dx;
            const f32 y = eval_curve(points, x);
            samples[s]  = to_screen({x, y});
        }
        m_->dl_.polyline(samples, m_->style_.accent, 2.0f, false);
        m_->dl_.pop_clip();
    }

    for (std::size_t i = 0; i < points.size(); ++i) {
        const curve_point& pt = points[i];
        const vec2 sp = to_screen({pt.x, pt.y});
        const id pk   = hash_id({reinterpret_cast<const char*>(&i), sizeof(i)}, key);
        const auto draw_tangent = [&](vec2 tang, const char* tag) {
            const vec2 tp = to_screen({pt.x + tang.x, pt.y + tang.y});
            const id   tk = hash_id(tag, pk);
            const bool active = m_->active_ == tk || rect{{tp.x - 5.0f, tp.y - 5.0f}, {tp.x + 5.0f, tp.y + 5.0f}}.contains(m_->input_.mouse_);
            m_->dl_.line(sp, tp, m_->style_.text_dim.scaled_alpha(0.65f), 1.0f);
            m_->dl_.circle_filled(tp, 3.0f, active ? m_->style_.accent_hover : m_->style_.text_dim);
        };
        if (i > 0)                 { draw_tangent(pt.in_tangent, "##tin"); }
        if (i + 1 < points.size()) { draw_tangent(pt.out_tangent, "##tout"); }
        const bool p_active = m_->active_ == pk || rect{{sp.x - 6.0f, sp.y - 6.0f}, {sp.x + 6.0f, sp.y + 6.0f}}.contains(m_->input_.mouse_);
        m_->dl_.circle_filled(sp, 4.5f, p_active ? m_->style_.accent_hover : m_->style_.accent);
    }

    m_->last_item_key_  = key;
    m_->last_item_rect_ = box;
    return changed;
}

// ---- property inspector grid ---------------------------------------------------------------------------------------

bool context::begin_property_grid(std::string_view id_str, f32 label_ratio)
{
    if (m_->cur_ == nullptr || m_->prop_grid_.active) { return false; }
    (void)id_str;
    const f32 total = std::max(content_width(), 80.0f);
    const f32 r     = std::clamp(label_ratio, 0.15f, 0.75f);
    const f32 gap   = m_->style_.item_spacing;
    m_->prop_grid_  = {
        .active        = true,
        .label_w       = std::floor(total * r),
        .value_w       = std::max(total - std::floor(total * r) - gap, 40.0f),
        .origin_x      = m_->layout_.origin.x,
        .row_y         = m_->layout_.line_top,
        .row_read_only = false,
        .row_mixed     = false,
    };
    return true;
}

bool context::property_row_begin(std::string_view label, const property_options& opt)
{
    if (!m_->prop_grid_.active) { return false; }
    auto& pg = m_->prop_grid_;
    const font_id f = current_font();
    const f32 fh    = frame_height();
    const rect row_r = layout_place(*this, {pg.label_w, fh});
    pg.row_y         = row_r.min.y;
    pg.row_read_only = opt.read_only;
    pg.row_mixed     = opt.mixed;

    bool reset_clicked = false;
    f32 text_x = row_r.min.x;
    if (opt.modified) {
        const rect dot_r{{row_r.min.x, row_r.center().y - 6.0f}, {row_r.min.x + 12.0f, row_r.center().y + 6.0f}};
        const interaction di = interact(*this, hash_id("##reset", widget_id(*this, label)), dot_r);
        m_->dl_.circle_filled(dot_r.center(), di.hovered ? 4.0f : 3.0f, di.hovered ? m_->style_.accent_hover : m_->style_.accent);
        if (di.pressed) { reset_clicked = true; }
        text_x += 14.0f;
    }

    const std::string_view shown = visible_label(label);
    const rect lbl_clip{{text_x, row_r.min.y}, {row_r.max.x - 4.0f, row_r.max.y}};
    m_->dl_.push_clip(lbl_clip);
    m_->dl_.text({text_x, row_r.min.y + (fh - m_->font_.line_height(f)) * 0.5f},
                 opt.modified ? m_->style_.text : m_->style_.text_dim, shown, f);
    m_->dl_.pop_clip();

    if (!opt.tooltip.empty() && pointer_over(*this, row_r)) {
        tooltip(opt.tooltip);
    }

    same_line();
    set_next_item_width(pg.value_w);
    if (pg.row_read_only) { begin_disabled(true); }
    return reset_clicked;
}

void context::property_row_end()
{
    if (!m_->prop_grid_.active) { return; }
    auto& pg = m_->prop_grid_;
    if (pg.row_read_only) {
        end_disabled();
        pg.row_read_only = false;
    }
    if (pg.row_mixed) {
        const rect vr = item_rect();
        const font_id f = current_font();
        m_->dl_.text({vr.max.x - 14.0f, vr.min.y + (vr.height() - m_->font_.line_height(f)) * 0.5f},
                     m_->style_.warning, "~", f);
        pg.row_mixed = false;
    }
}

void context::end_property_grid()
{
    m_->prop_grid_ = {};
}

} // namespace strata
