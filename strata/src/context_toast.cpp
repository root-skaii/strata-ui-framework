// toasts: short notifications stacked in a corner of the display

#include "strata/context.hpp"

#include "context_impl.hpp"

#include <algorithm>
#include <cmath>

namespace strata {

color kind_color(toast_kind k, const style& st) noexcept
{
    switch (k) {
    case toast_kind::success: return st.success;
    case toast_kind::warning: return st.warning;
    case toast_kind::error:   return st.error;
    default:                  return st.accent_hover;
    }
}

namespace {

[[nodiscard]] std::string_view kind_name(toast_kind k) noexcept
{
    switch (k) {
    case toast_kind::success: return "done";
    case toast_kind::warning: return "warning";
    case toast_kind::error:   return "error";
    default:                  return {};
    }
}

} // namespace

toast_handle context::toast(std::string_view title, std::string_view text, toast_kind kind, f32 seconds)
{
    toast_options o;
    o.title   = title;
    o.text    = text;
    o.kind    = kind;
    o.seconds = seconds;
    return toast(o);
}

toast_handle context::toast(const toast_options& o)
{
    toast_entry t;
    t.title    = std::string{o.title};
    t.text     = std::string{o.text};
    t.kind     = o.kind;
    t.sticky   = o.seconds <= 0.0f;
    t.duration = t.sticky ? 0.0f : std::max(o.seconds, 0.5f);
    t.seq      = ++m_->toast_seq_;
    t.progress = o.progress;
    for (const std::string_view a : o.actions) {
        if (t.actions.size() < 3) { t.actions.emplace_back(a); }
    }
    if (t.title.empty()) { t.title = std::string{kind_name(o.kind)}; }
    const toast_handle h = t.seq;
    m_->toasts_.push_back(std::move(t));
    constexpr std::size_t max_toasts = 8;
    if (m_->toasts_.size() > max_toasts) {
        m_->toasts_.erase(m_->toasts_.begin());
    }
    return h;
}

int context::toast_action(toast_handle h)
{
    for (std::size_t i = 0; i < m_->toast_results_.size(); ++i) {
        if (m_->toast_results_[i].first == h) {
            const int a = m_->toast_results_[i].second;
            m_->toast_results_.erase(m_->toast_results_.begin() + static_cast<std::ptrdiff_t>(i));
            return a;
        }
    }
    return -1;
}

void context::toast_progress(toast_handle h, f32 fraction, std::string_view text)
{
    for (toast_entry& t : m_->toasts_) {
        if (t.seq != h || t.dismissed) { continue; }
        if (!text.empty()) { t.text = std::string{text}; }
        if (fraction == toast_busy) {
            t.progress = toast_busy;
        } else {
            t.progress = std::clamp(fraction, 0.0f, 1.0f);
            if (t.progress >= 1.0f && t.sticky) { // done: it stays a moment, then goes
                t.sticky   = false;
                t.age      = 0.0f;
                t.duration = 2.0f;
            }
        }
    }
}

void context::toast_close(toast_handle h)
{
    for (toast_entry& t : m_->toasts_) {
        if (t.seq == h) { t.dismissed = true; }
    }
}

bool context::toast_alive(toast_handle h) const noexcept
{
    for (const toast_entry& t : m_->toasts_) {
        if (t.seq == h && !t.dismissed) { return true; }
    }
    return false;
}

void context::toast_end_frame()
{
    if (m_->toasts_.empty()) {
        return;
    }
    const font_id f  = 0;
    const f32 lh     = m_->font_.line_height(f);
    const f32 width  = std::min(340.0f, m_->display_.x * 0.45f);
    const f32 pad    = 12.0f;
    const f32 margin = 14.0f;
    const f32 gap    = 8.0f;
    const f32 btn_h  = lh + 8.0f;
    const bool at_bottom = m_->toast_corner_ == screen_corner::bottom_right || m_->toast_corner_ == screen_corner::bottom_left;
    const bool at_right  = m_->toast_corner_ == screen_corner::top_right || m_->toast_corner_ == screen_corner::bottom_right;

    const u32 previous_owner = m_->run_owner_;
    switch_run(run_overlay);
    const bool saved_overlay = m_->in_overlay_;
    m_->in_overlay_ = true; // toasts are popup content: they get the pointer even where a window would
    m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});

    f32 cursor = at_bottom ? m_->display_.y - margin : margin;
    for (std::size_t n = m_->toasts_.size(); n-- > 0;) { // newest nearest the corner
        toast_entry& t = m_->toasts_[n];

        const bool  has_bar   = t.progress >= 0.0f || t.progress == toast_busy;
        const bool  has_acts  = !t.actions.empty();
        const bool  plain     = !has_bar && !has_acts && t.sticky == false;
        const f32  inner_w = width - 2.0f * pad - 6.0f;
        const vec2 body_size = t.text.empty() ? vec2{} : rich_layout(t.text, f, m_->style_.text, inner_w, false);
        // (rich_layout's lines are drawn right after measuring, before the next toast)
        const f32 title_h = t.title.empty() ? 0.0f : lh;
        f32 height  = pad * 2.0f + title_h + (t.title.empty() || t.text.empty() ? 0.0f : 3.0f) + body_size.y + 3.0f;
        if (has_bar)  { height += 9.0f; }
        if (has_acts) { height += btn_h + 8.0f; }

        t.anim = approach(t.anim, t.dismissed ? 0.0f : 1.0f, 14.0f);
        const f32 eased  = t.anim * t.anim * (3.0f - 2.0f * t.anim);
        const f32 fade_out = t.sticky ? 1.0f : std::clamp((t.duration - t.age) / 0.30f, 0.0f, 1.0f);
        const f32 alpha  = std::min(eased, fade_out);
        const f32 x = at_right ? m_->display_.x - margin - width + (1.0f - eased) * 36.0f : margin - (1.0f - eased) * 36.0f;
        const f32 y = at_bottom ? cursor - height : cursor;
        const rect r = rect::from_size({x, y}, {width, height});

        m_->dl_.push_alpha(alpha);
        const id key = hash_id("##toast", static_cast<id>(t.seq));

        // the controls first: they claim a press before the body can
        const rect close_r = rect::from_size({r.max.x - 26.0f, r.min.y + 6.0f}, {20.0f, 20.0f});
        const bool over_body = r.contains(m_->mouse_);
        interaction close_in{};
        if (!plain && !t.dismissed) { close_in = interact(hash_id("##toastx", static_cast<id>(t.seq)), close_r); }
        std::array<rect, 3> btn{};
        std::array<interaction, 3> btn_in{};
        if (has_acts) {
            f32 bx = r.min.x + pad + 6.0f;
            const f32 by = r.max.y - pad - btn_h + 2.0f;
            for (std::size_t i = 0; i < t.actions.size(); ++i) {
                const f32 bw = m_->font_.measure(f, t.actions[i]).x + 22.0f;
                btn[i] = rect::from_size({bx, by}, {bw, btn_h});
                bx += bw + 6.0f;
                if (!t.dismissed) { btn_in[i] = interact(hash_id("##toastb", static_cast<id>(t.seq * 4 + i)), btn[i]); }
            }
        }
        const interaction in = interact(key, r);
        if (in.hovered) { m_->toast_hover_cur_ = true; }
        if (in.pressed && plain) { t.dismissed = true; }
        if (close_in.pressed) { t.dismissed = true; }
        for (std::size_t i = 0; i < t.actions.size(); ++i) {
            if (btn_in[i].pressed && !t.dismissed) {
                t.dismissed = true;
                if (m_->toast_results_.size() >= 16) { m_->toast_results_.erase(m_->toast_results_.begin()); }
                m_->toast_results_.emplace_back(t.seq, static_cast<int>(i));
            }
        }
        const bool touched = over_body || in.hovered || in.held;
        t.paused = touched;
        if (!touched && !t.sticky) { t.age += m_->wall_dt_; } // hovering pauses the timer
        if (has_bar && t.progress == toast_busy) { t.busy_phase += m_->dt_; }

        const color accent = kind_color(t.kind, m_->style_);
        shape_style body;
        body.radius        = radii(m_->style_.rounding * 0.8f);
        body.fill_top      = lerp(color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 255}, m_->style_.widget_hover, in.hovered && plain ? 0.35f : 0.0f);
        body.fill_bottom   = body.fill_top;
        body.border        = m_->style_.border;
        body.border_width  = m_->style_.border_width;
        body.shadow        = m_->style_.shadow;
        body.shadow_blur   = m_->style_.shadow_blur * 0.8f;
        body.shadow_offset = {0.0f, m_->style_.shadow_blur * 0.3f};
        popup_panel(r, body);

        shape_style bar; // the kind's color down the left edge
        bar.radius      = radii(2.0f);
        bar.fill_top    = accent;
        bar.fill_bottom = accent;
        m_->dl_.shape({{r.min.x + 6.0f, r.min.y + 8.0f}, {r.min.x + 9.0f, r.max.y - 8.0f}}, bar);

        f32 ty = r.min.y + pad;
        if (!t.title.empty()) {
            m_->dl_.text({r.min.x + pad + 6.0f, ty}, accent, t.title, f);
            ty += title_h + (t.text.empty() ? 0.0f : 3.0f);
        }
        if (!t.text.empty()) {
            rich_layout(t.text, f, m_->style_.text, inner_w, false);
            rich_draw({r.min.x + pad + 6.0f, ty});
        }
        ty += body_size.y + 3.0f;

        if (has_bar) { // progress: a track and its fill, or an endless bar sliding across it
            const rect track = {{r.min.x + pad + 6.0f, ty + 1.0f}, {r.max.x - pad, ty + 6.0f}};
            shape_style tr;
            tr.radius = radii(2.5f);
            tr.fill_top = tr.fill_bottom = m_->style_.widget_bg;
            m_->dl_.shape(track, tr);
            shape_style fill;
            fill.radius = radii(2.5f);
            fill.fill_top = fill.fill_bottom = accent;
            if (t.progress == toast_busy) {
                const f32 span = track.width() * 0.35f;
                const f32 phase = std::fmod(t.busy_phase * 0.9f, 1.0f);
                const f32 x0 = track.min.x - span + (track.width() + span) * phase;
                m_->dl_.push_clip({{track.min.x, track.min.y - 1.0f}, {track.max.x, track.max.y + 1.0f}});
                m_->dl_.shape({{x0, track.min.y}, {x0 + span, track.max.y}}, fill);
                m_->dl_.pop_clip();
            } else if (t.progress > 0.0f) {
                m_->dl_.shape({track.min, {track.min.x + std::max(track.width() * t.progress, 5.0f), track.max.y}}, fill);
            }
            ty += 9.0f;
        }
        if (has_acts) {
            for (std::size_t i = 0; i < t.actions.size(); ++i) {
                shape_style b;
                b.radius = radii(m_->style_.rounding * 0.6f);
                b.fill_top = b.fill_bottom = lerp(m_->style_.widget_bg, m_->style_.widget_hover, btn_in[i].hovered ? 1.0f : 0.0f);
                b.border = btn_in[i].hovered ? accent : m_->style_.widget_border;
                b.border_width = 1.0f;
                m_->dl_.shape(btn[i], b);
                const vec2 ts = m_->font_.measure(f, t.actions[i]);
                m_->dl_.text({btn[i].min.x + (btn[i].width() - ts.x) * 0.5f, btn[i].min.y + (btn_h - ts.y) * 0.5f}, m_->style_.text, t.actions[i], f);
            }
        }
        if (!plain && (over_body || close_in.hovered)) { // a small x to dismiss it
            const vec2 c = close_r.center();
            const color xc = close_in.hovered ? m_->style_.text : m_->style_.text_dim;
            m_->dl_.line({c.x - 3.5f, c.y - 3.5f}, {c.x + 3.5f, c.y + 3.5f}, xc, 1.5f);
            m_->dl_.line({c.x - 3.5f, c.y + 3.5f}, {c.x + 3.5f, c.y - 3.5f}, xc, 1.5f);
        }
        // how much time is left (a plain or timed toast; a progress bar shows its own)
        if (!t.sticky && !has_bar) {
            const f32 left = std::clamp(1.0f - t.age / t.duration, 0.0f, 1.0f);
            m_->dl_.rect_filled({{r.min.x + 12.0f, r.max.y - 5.0f}, {r.min.x + 12.0f + (width - 24.0f) * left, r.max.y - 3.0f}}, accent.scaled_alpha(0.55f), 1.0f);
        }
        m_->dl_.pop_alpha();

        cursor += at_bottom ? -(height + gap) : (height + gap);
    }
    m_->dl_.pop_clip();
    m_->in_overlay_ = saved_overlay;
    switch_run(previous_owner);

    std::erase_if(m_->toasts_, [](const toast_entry& t) { return (!t.sticky && t.age >= t.duration) || (t.dismissed && t.anim < 0.02f); });
}

} // namespace strata
