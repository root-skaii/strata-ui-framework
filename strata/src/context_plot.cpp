// plots: line charts, histograms, sparklines and multi-series charts

#include "strata/context.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace strata {

namespace {

constexpr std::array<u32, 6> series_palette = {0x5b8dffffu, 0x19c2b4ffu, 0xffb454ffu, 0xf0568fffu, 0xa78bfaffu, 0x7bc74dffu};

[[nodiscard]] color series_color(std::size_t i, color given, const style& st) noexcept
{
    if (given.a != 0) { return given; }
    if (i == 0) { return st.accent; }
    return color::from_hex(series_palette[i % series_palette.size()]);
}

[[nodiscard]] std::string number_text(f32 v, f32 span)
{
    std::array<char, 32> buf;
    const int decimals = span >= 100.0f ? 0 : (span >= 10.0f ? 1 : (span >= 1.0f ? 2 : 3));
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, decimals);
    return {buf.data(), r.ptr};
}

// a step of 1, 2 or 5 times a power of ten that puts about `target_count` ticks on `range`
[[nodiscard]] f32 nice_step(f32 range, f32 target_count) noexcept
{
    const f32 raw  = std::max(range, 1.0e-12f) / std::max(target_count, 1.0f);
    const f32 mag  = std::pow(10.0f, std::floor(std::log10(raw)));
    const f32 norm = raw / mag;
    const f32 nice = norm < 1.5f ? 1.0f : (norm < 3.5f ? 2.0f : (norm < 7.5f ? 5.0f : 10.0f));
    return nice * mag;
}

[[nodiscard]] std::string tick_text(f32 v, f32 step, std::string_view unit)
{
    const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(std::max(step, 1.0e-9f)) - 1.0e-4f)), 0, 6);
    if (std::abs(v) < step * 1.0e-3f) { v = 0.0f; } // no "-0"
    std::array<char, 40> buf;
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, decimals);
    std::string out{buf.data(), r.ptr};
    if (!unit.empty()) {
        if (unit.front() != '%') { out += ' '; }
        out.append(unit);
    }
    return out;
}

} // namespace

void context::draw_tooltip_at(vec2 anchor, std::string_view text)
{
    if (in_overlay_ || text.empty()) {
        return;
    }
    const font_id f = current_font();
    const vec2 ts   = label_size(f, text);
    const f32 padx  = 9.0f;
    const f32 pady  = 6.0f;
    const vec2 size{ts.x + 2.0f * padx, ts.y + 2.0f * pady};

    vec2 pos = anchor;
    if (pos.x + size.x > display_.x - 4.0f) { pos.x = display_.x - 4.0f - size.x; }
    if (pos.y + size.y > display_.y - 4.0f) { pos.y = anchor.y - size.y - 30.0f; }
    pos.x = std::max(pos.x, 4.0f);
    pos.y = std::max(pos.y, 4.0f);

    const u32 previous_owner = run_owner_;
    switch_run(run_overlay);
    dl_.push_clip_absolute({{0.0f, 0.0f}, display_});
    shape_style body;
    body.radius        = radii(style_.rounding * 0.6f);
    body.fill_top      = color{style_.window_bg.r, style_.window_bg.g, style_.window_bg.b, 255};
    body.fill_bottom   = body.fill_top;
    body.border        = style_.border;
    body.border_width  = style_.border_width;
    body.shadow        = style_.shadow;
    body.shadow_blur   = style_.shadow_blur * 0.5f;
    body.shadow_offset = {0.0f, 3.0f};
    popup_panel(rect::from_size(pos, size), body);
    label_draw({pos.x + padx, pos.y + pady}, style_.text, text, f);
    dl_.pop_clip();
    switch_run(previous_owner);
}

void context::plot_impl(std::string_view label, std::span<const plot_series> series, vec2 size, plot_kind kind, f32 lo, f32 hi,
                        u32 offset, std::string_view overlay, bool compact)
{
    if (cur_ == nullptr || series.empty()) {
        return;
    }
    const font_id f  = current_font();
    const f32     lh = font_.line_height(f);

    std::size_t n = series[0].values.size();
    for (const plot_series& s : series) { n = std::min(n, s.values.size()); }
    const auto sample = [&](const plot_series& s, std::size_t i) -> f32 {
        return s.values[(static_cast<std::size_t>(offset) + i) % s.values.size()];
    };

    if (std::isnan(lo) || std::isnan(hi)) {
        f32 mn = std::numeric_limits<f32>::max();
        f32 mx = std::numeric_limits<f32>::lowest();
        for (const plot_series& s : series) {
            for (std::size_t i = 0; i < n; ++i) {
                const f32 v = sample(s, i);
                if (std::isfinite(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
            }
        }
        if (mn > mx) { mn = 0.0f; mx = 1.0f; }
        if (kind == plot_kind::histogram) { mn = std::min(mn, 0.0f); }
        if (mx - mn < 1.0e-6f) { mx = mn + 1.0f; }
        const f32 pad = (mx - mn) * 0.05f;
        if (std::isnan(lo)) { lo = kind == plot_kind::histogram && mn == 0.0f ? 0.0f : mn - pad; }
        if (std::isnan(hi)) { hi = mx + pad; }
    }
    if (hi - lo < 1.0e-6f) { hi = lo + 1.0f; }

    const f32 h = size.y > 0.0f ? size.y : (compact ? 20.0f : 80.0f);
    if (size.x > 0.0f) { layout_.next_width = size.x; }
    const field_layout fl = layout_field(compact ? std::string_view{} : visible_label(label), h);
    const rect box = fl.control;

    const id key = hash_id(label, current_seed());
    const interaction in = compact ? interaction{} : interact(key, box);

    rect inner = box;
    if (!compact) {
        shape_style bg;
        bg.radius       = radii(style_.rounding * 0.8f);
        bg.fill_top     = color{0, 0, 0, 60};
        bg.fill_bottom  = color{0, 0, 0, 60};
        bg.border       = style_.widget_border;
        bg.border_width = style_.border_width;
        dl_.shape(box, bg);

        const std::string hi_text = number_text(hi, hi - lo);
        const std::string lo_text = number_text(lo, hi - lo);
        const bool axis = h >= lh * 3.0f;
        f32 label_w = 0.0f;
        if (axis) {
            label_w = std::max(font_.measure(f, hi_text).x, font_.measure(f, lo_text).x) + 8.0f;
            dl_.text({box.min.x + 6.0f, box.min.y + 3.0f}, style_.text_dim, hi_text, f);
            dl_.text({box.min.x + 6.0f, box.max.y - 3.0f - lh}, style_.text_dim, lo_text, f);
        }
        inner = {{box.min.x + 6.0f + label_w, box.min.y + 6.0f}, {box.max.x - 6.0f, box.max.y - 6.0f}};
        for (int g = 0; g < 3; ++g) { // grid: bottom, middle, top
            const f32 y = std::round(inner.max.y - inner.height() * static_cast<f32>(g) * 0.5f);
            dl_.rect_filled({{inner.min.x, y}, {inner.max.x, y + 1.0f}}, color{255, 255, 255, 16});
        }
    }
    const f32 span = std::max(inner.height(), 1.0f);
    const auto y_of = [&](f32 v) { return inner.max.y - std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) * span; };

    const f32 width_px = std::max(inner.width() * scale_, 1.0f);
    if (n > 0) {
        const f32 dx = n > 1 ? inner.width() / static_cast<f32>(n - 1) : 0.0f;
        dl_.push_clip(inner.expanded(3.0f));
        for (std::size_t si = 0; si < series.size(); ++si) {
            const color c = series_color(si, series[si].col, style_);
            if (kind == plot_kind::lines) {
                plot_scratch_.clear();
                if (static_cast<f32>(n) > width_px * 2.0f) { // more samples than pixels: keep each bucket's extremes
                    const std::size_t buckets = static_cast<std::size_t>(width_px);
                    for (std::size_t b = 0; b < buckets; ++b) {
                        const std::size_t from = b * n / buckets;
                        const std::size_t to   = std::max(from + 1, (b + 1) * n / buckets);
                        std::size_t imin = from, imax = from;
                        for (std::size_t i = from; i < to && i < n; ++i) {
                            if (sample(series[si], i) < sample(series[si], imin)) { imin = i; }
                            if (sample(series[si], i) > sample(series[si], imax)) { imax = i; }
                        }
                        const std::size_t first = std::min(imin, imax);
                        const std::size_t second = std::max(imin, imax);
                        const f32 x = inner.min.x + inner.width() * static_cast<f32>(b) / static_cast<f32>(std::max<std::size_t>(buckets - 1, 1));
                        plot_scratch_.push_back({x, y_of(sample(series[si], first))});
                        if (second != first) { plot_scratch_.push_back({x, y_of(sample(series[si], second))}); }
                    }
                } else {
                    for (std::size_t i = 0; i < n; ++i) {
                        plot_scratch_.push_back({n > 1 ? inner.min.x + dx * static_cast<f32>(i) : inner.center().x, y_of(sample(series[si], i))});
                    }
                }
                if (plot_scratch_.size() >= 2) {
                    dl_.polyline(plot_scratch_, c, compact ? 1.25f : 1.6f, false);
                } else if (plot_scratch_.size() == 1) {
                    dl_.circle_filled(plot_scratch_[0], 2.5f, c);
                }
            } else {
                const f32 base = y_of(std::clamp(0.0f, lo, hi));
                const std::size_t buckets = std::min(n, static_cast<std::size_t>(width_px));
                const f32 bw = inner.width() / static_cast<f32>(buckets);
                const f32 sub = series.size() > 1 ? bw / static_cast<f32>(series.size()) : bw;
                for (std::size_t b = 0; b < buckets; ++b) {
                    const std::size_t from = b * n / buckets;
                    const std::size_t to   = std::max(from + 1, (b + 1) * n / buckets);
                    f32 v = sample(series[si], from);
                    for (std::size_t i = from; i < to && i < n; ++i) { v = std::max(v, sample(series[si], i)); }
                    const f32 x0 = inner.min.x + bw * static_cast<f32>(b) + sub * static_cast<f32>(si);
                    const f32 gap = sub > 3.0f ? 1.0f : 0.0f;
                    const f32 y = y_of(v);
                    dl_.rect_filled({{x0, std::min(y, base)}, {x0 + sub - gap, std::max(std::max(y, base), std::min(y, base) + 1.0f)}}, c, 1.0f);
                }
            }
        }
        dl_.pop_clip();
    }

    if (compact) {
        return;
    }

    // legend of multi-series charts, and the overlay text
    if (series.size() > 1) {
        f32 x = box.max.x - 8.0f;
        for (std::size_t si = series.size(); si-- > 0;) {
            const std::string_view name = series[si].name.empty() ? std::string_view{"series"} : series[si].name;
            const f32 tw = font_.measure(f, name).x;
            x -= tw;
            dl_.text({x, box.min.y + 3.0f}, style_.text_dim, name, f);
            x -= 12.0f;
            dl_.circle_filled({x + 4.0f, box.min.y + 3.0f + lh * 0.5f}, 3.5f, series_color(si, series[si].col, style_));
            x -= 10.0f;
        }
    }
    if (!overlay.empty()) {
        const vec2 ts = font_.measure(f, overlay);
        dl_.text({inner.min.x + (inner.width() - ts.x) * 0.5f, box.min.y + 3.0f}, style_.text_dim, overlay, f);
    }

    if (in.hovered && n > 0 && inner.width() > 1.0f) {
        const f32 t = std::clamp((mouse_.x - inner.min.x) / inner.width(), 0.0f, 1.0f);
        const std::size_t idx = kind == plot_kind::lines ? static_cast<std::size_t>(std::lround(t * static_cast<f32>(n - 1)))
                                                          : std::min(n - 1, static_cast<std::size_t>(t * static_cast<f32>(n)));
        const f32 x = kind == plot_kind::lines && n > 1 ? inner.min.x + inner.width() * static_cast<f32>(idx) / static_cast<f32>(n - 1)
                                                        : inner.min.x + inner.width() * (static_cast<f32>(idx) + 0.5f) / static_cast<f32>(n);
        dl_.rect_filled({{std::round(x), inner.min.y}, {std::round(x) + 1.0f, inner.max.y}}, color{255, 255, 255, 90});
        std::string tip = "#" + std::to_string(idx);
        for (std::size_t si = 0; si < series.size(); ++si) {
            const f32 v = sample(series[si], idx);
            if (kind == plot_kind::lines) { dl_.circle_filled({x, y_of(v)}, 3.5f, series_color(si, series[si].col, style_)); }
            tip += '\n';
            if (!series[si].name.empty()) { tip.append(series[si].name); tip += ": "; }
            tip += number_text(v, hi - lo);
        }
        draw_tooltip_at(mouse_ + vec2{14.0f, 20.0f}, tip);
    }
}

context::chart_view& context::chart_view_for(id key) noexcept
{
    chart_view* oldest = &chart_views_[0];
    for (chart_view& v : chart_views_) {
        if (v.key == key) { v.last_frame = frame_; return v; }
        if (v.last_frame < oldest->last_frame) { oldest = &v; }
    }
    *oldest = {};
    oldest->key        = key;
    oldest->last_frame = frame_;
    return *oldest;
}

bool context::plot_zoomed(std::string_view label) const
{
    const id key = hash_id(label, current_seed());
    for (const chart_view& v : chart_views_) {
        if (v.key == key) { return v.x_set || v.y_set; }
    }
    return false;
}

vec2 context::plot_x_range(std::string_view label) const
{
    const id key = hash_id(label, current_seed());
    for (const chart_view& v : chart_views_) {
        if (v.key == key) { return {v.sx_lo, v.sx_hi}; }
    }
    return {};
}

void context::plot_reset_view(std::string_view label)
{
    const id key = hash_id(label, current_seed());
    for (chart_view& v : chart_views_) {
        if (v.key == key) { v.x_set = v.y_set = false; }
    }
}

// a chart with axes: tick marks and labels with units, grid, area fills, wheel zoom and drag pan
void context::chart_impl(std::string_view label, std::span<const plot_series> series, const plot_options& o)
{
    if (cur_ == nullptr || series.empty()) {
        return;
    }
    const font_id f  = current_font();
    const f32     lh = font_.line_height(f);
    const bool    hist = o.kind == plot_kind::histogram;

    std::size_t n = series[0].values.size();
    for (const plot_series& sr : series) { n = std::min(n, sr.values.size()); }
    const f32 x_step = o.x_step > 0.0f ? o.x_step : 1.0f;
    const auto sample = [&](const plot_series& sr, std::size_t i) -> f32 {
        return sr.values[(static_cast<std::size_t>(o.offset) + i) % sr.values.size()];
    };

    // layout: the control, and inside it the bands for the axes
    const f32 h = o.size.y > 0.0f ? o.size.y : 160.0f;
    if (o.size.x > 0.0f) { layout_.next_width = o.size.x; }
    const field_layout fl = layout_field(visible_label(label), h);
    const rect box = fl.control;
    const id   key = hash_id(label, current_seed());
    const interaction in = interact(key, box);
    chart_view& view = chart_view_for(key);

    // the x range: the data (or the given one), or what the user zoomed to
    const f32 data_lo = o.x.lo, data_hi = o.x.hi;
    const f32 xd0 = std::isnan(data_lo) ? o.x_start : data_lo;
    f32       xd1 = std::isnan(data_hi) ? o.x_start + static_cast<f32>(n > 1 ? n - 1 : 1) * x_step : data_hi;
    if (hist && std::isnan(data_hi)) { xd1 = o.x_start + static_cast<f32>(std::max<std::size_t>(n, 1)) * x_step; } // bars have width
    if (xd1 - xd0 < 1.0e-9f) { xd1 = xd0 + 1.0f; }

    // interaction with last frame's picture of the chart
    if (o.zoom_pan && view.inner.width() > 1.0f && view.sx_hi > view.sx_lo) {
        const rect ir = view.inner;
        const f32 sx0 = view.sx_lo, sx1 = view.sx_hi, sy0 = view.sy_lo, sy1 = view.sy_hi;
        const bool over = in.hovered && ir.contains(mouse_);
        if (over && wheel_ != 0.0f && !wheel_consumed_) {
            wheel_consumed_ = true;
            const f32 k = std::pow(0.85f, wheel_);
            if (mod_ctrl_) {
                const f32 m = sy1 - (mouse_.y - ir.min.y) / std::max(ir.height(), 1.0f) * (sy1 - sy0);
                view.y_lo = m - (m - sy0) * k;
                view.y_hi = m + (sy1 - m) * k;
                view.y_set = true;
            } else {
                const f32 m = sx0 + (mouse_.x - ir.min.x) / std::max(ir.width(), 1.0f) * (sx1 - sx0);
                view.x_lo = m - (m - sx0) * k;
                view.x_hi = m + (sx1 - m) * k;
                view.x_set = true;
            }
        }
        if (in.held && (mouse_delta_.x != 0.0f || mouse_delta_.y != 0.0f)) {
            const f32 dx = -mouse_delta_.x / ir.width() * (sx1 - sx0);
            view.x_lo = sx0 + dx;
            view.x_hi = sx1 + dx;
            view.x_set = true;
            if (view.y_set) {
                const f32 dy = mouse_delta_.y / std::max(ir.height(), 1.0f) * (sy1 - sy0);
                view.y_lo = sy0 + dy;
                view.y_hi = sy1 + dy;
            }
        }
        if (over && mouse_pressed_) { // double-click: back to the whole data
            if (time_ - view.last_click < 0.35) { view.x_set = view.y_set = false; }
            view.last_click = time_;
        }
    }
    f32 xv0 = xd0, xv1 = xd1;
    if (view.x_set) {
        const f32 dspan = xd1 - xd0;
        f32 span = std::clamp(view.x_hi - view.x_lo, std::min(dspan, x_step * 2.0f), dspan * 4.0f);
        const f32 keep = 0.2f * std::min(span, dspan); // the view always overlaps the data by this much
        f32 lo = std::clamp(view.x_lo, xd0 - span + keep, xd1 - keep);
        view.x_lo = lo;
        view.x_hi = lo + span;
        xv0 = view.x_lo;
        xv1 = view.x_hi;
    }

    std::size_t i0 = 0, i1 = n > 0 ? n - 1 : 0;
    if (n > 0) {
        const f32 a = std::floor((xv0 - o.x_start) / x_step) - 1.0f;
        const f32 b = std::ceil((xv1 - o.x_start) / x_step) + 1.0f;
        i0 = static_cast<std::size_t>(std::clamp(a, 0.0f, static_cast<f32>(n - 1)));
        i1 = static_cast<std::size_t>(std::clamp(b, static_cast<f32>(i0), static_cast<f32>(n - 1)));
    }

    // the y range: fixed, zoomed, or the visible samples
    f32 yv0 = o.y.lo, yv1 = o.y.hi;
    if (view.y_set) {
        yv0 = view.y_lo;
        yv1 = view.y_hi;
    } else if (std::isnan(yv0) || std::isnan(yv1)) {
        f32 mn = std::numeric_limits<f32>::max();
        f32 mx = std::numeric_limits<f32>::lowest();
        for (const plot_series& sr : series) {
            for (std::size_t i = i0; i <= i1 && n > 0; ++i) {
                const f32 v = sample(sr, i);
                if (std::isfinite(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
            }
        }
        if (mn > mx) { mn = 0.0f; mx = 1.0f; }
        if (hist) { mn = std::min(mn, 0.0f); }
        if (mx - mn < 1.0e-6f) { mx = mn + 1.0f; }
        const f32 pad = (mx - mn) * 0.06f;
        if (std::isnan(yv0)) { yv0 = hist && mn == 0.0f ? 0.0f : mn - pad; }
        if (std::isnan(yv1)) { yv1 = mx + pad; }
    }
    if (yv1 - yv0 < 1.0e-9f) { yv1 = yv0 + 1.0f; }

    const bool  legend = series.size() > 1;
    const bool  header = !o.y.title.empty() || !o.y.unit.empty() || legend;
    const f32   top    = 6.0f + (header ? lh + 4.0f : 0.0f);
    f32 bottom = 6.0f;
    f32 left   = 6.0f;
    if (o.ticks) {
        const f32 guess = (yv1 - yv0) / 5.0f;
        left   = 12.0f + std::max(font_.measure(f, tick_text(yv0, guess, o.y.unit)).x, font_.measure(f, tick_text(yv1, guess, o.y.unit)).x);
        bottom = 6.0f + lh + 4.0f;
    }
    if (!o.x.title.empty()) { bottom += lh + 2.0f; }
    const rect inner = {{box.min.x + left, box.min.y + top}, {box.max.x - 10.0f, box.max.y - bottom}};

    shape_style bg;
    bg.radius       = radii(style_.rounding * 0.8f);
    bg.fill_top     = color{0, 0, 0, 60};
    bg.fill_bottom  = color{0, 0, 0, 60};
    bg.border       = style_.widget_border;
    bg.border_width = style_.border_width;
    dl_.shape(box, bg);
    if (inner.width() < 8.0f || inner.height() < 8.0f) {
        return;
    }

    const f32 kx = inner.width() / (xv1 - xv0);
    const f32 ky = inner.height() / (yv1 - yv0);
    const auto x_of = [&](f32 xv) { return inner.min.x + (xv - xv0) * kx; };
    const auto y_of = [&](f32 v) { return inner.max.y - std::clamp((v - yv0) * ky, -2.0f, inner.height() + 2.0f); };

    if (o.ticks) {
        const color grid = color{255, 255, 255, 18};
        const color axis = color{255, 255, 255, 70};
        const f32 ystep = nice_step(yv1 - yv0, std::max(inner.height() / 27.0f, 1.0f));
        for (long long k = static_cast<long long>(std::ceil(yv0 / ystep)); static_cast<f32>(k) * ystep <= yv1 + ystep * 1.0e-3f && k < 200000; ++k) {
            const f32 v = static_cast<f32>(k) * ystep;
            const f32 y = std::round(y_of(v));
            dl_.rect_filled({{inner.min.x, y}, {inner.max.x, y + 1.0f}}, grid);
            dl_.rect_filled({{inner.min.x - 3.0f, y}, {inner.min.x, y + 1.0f}}, axis);
            const std::string t = tick_text(v, ystep, o.y.unit);
            dl_.text({inner.min.x - 6.0f - font_.measure(f, t).x, y - lh * 0.5f}, style_.text_dim, t, f);
        }
        f32 widest = 0.0f;
        for (const f32 v : {xv0, xv1}) { widest = std::max(widest, font_.measure(f, tick_text(v, (xv1 - xv0) / 5.0f, o.x.unit)).x); }
        const f32 xstep = nice_step(xv1 - xv0, std::max(inner.width() / (widest + 28.0f), 1.0f));
        f32 last_right = -1.0e9f;
        for (long long k = static_cast<long long>(std::ceil(xv0 / xstep)); static_cast<f32>(k) * xstep <= xv1 + xstep * 1.0e-3f && k < 200000; ++k) {
            const f32 v = static_cast<f32>(k) * xstep;
            const f32 x = std::round(x_of(v));
            dl_.rect_filled({{x, inner.min.y}, {x + 1.0f, inner.max.y}}, color{255, 255, 255, 12});
            dl_.rect_filled({{x, inner.max.y}, {x + 1.0f, inner.max.y + 3.0f}}, axis);
            const std::string t = tick_text(v, xstep, o.x.unit);
            const f32 tw = font_.measure(f, t).x;
            const f32 tx = x - tw * 0.5f;
            if (tx > last_right + 6.0f && tx >= box.min.x + 2.0f && tx + tw <= box.max.x - 2.0f) {
                dl_.text({tx, inner.max.y + 5.0f}, style_.text_dim, t, f);
                last_right = tx + tw;
            }
        }
        dl_.rect_filled({{inner.min.x, inner.min.y}, {inner.min.x + 1.0f, inner.max.y}}, axis);
        dl_.rect_filled({{inner.min.x, inner.max.y}, {inner.max.x, inner.max.y + 1.0f}}, axis);
    }

    if (header) {
        std::string title{o.y.title};
        if (!o.y.unit.empty()) { title += title.empty() ? std::string{o.y.unit} : " (" + std::string{o.y.unit} + ")"; }
        dl_.text({box.min.x + 8.0f, box.min.y + 4.0f}, style_.text_dim, title, f);
        if (legend) {
            f32 x = box.max.x - 8.0f;
            for (std::size_t si = series.size(); si-- > 0;) {
                const std::string_view name = series[si].name.empty() ? std::string_view{"series"} : series[si].name;
                x -= font_.measure(f, name).x;
                dl_.text({x, box.min.y + 4.0f}, style_.text_dim, name, f);
                x -= 12.0f;
                dl_.circle_filled({x + 4.0f, box.min.y + 4.0f + lh * 0.5f}, 3.5f, series_color(si, series[si].col, style_));
                x -= 10.0f;
            }
        }
    }
    if (!o.x.title.empty()) {
        const f32 tw = font_.measure(f, o.x.title).x;
        dl_.text({inner.min.x + (inner.width() - tw) * 0.5f, box.max.y - 4.0f - lh}, style_.text_dim, o.x.title, f);
    }

    const f32 width_px = std::max(inner.width() * scale_, 1.0f);
    const f32 base_y   = y_of(std::clamp(0.0f, yv0, yv1));
    const std::size_t count = n > 0 ? i1 - i0 + 1 : 0;
    dl_.push_clip(inner.expanded(2.0f));
    for (int pass = 0; pass < 2 && count > 0; ++pass) { // pass 0: fills, pass 1: lines / bars
        for (std::size_t si = 0; si < series.size(); ++si) {
            const color c = series_color(si, series[si].col, style_);
            if (!hist) {
                if (pass == 0 && !o.fill) { continue; }
                plot_scratch_.clear();
                if (static_cast<f32>(count) > width_px * 2.0f) { // more samples than pixels: keep each bucket's extremes
                    const std::size_t buckets = static_cast<std::size_t>(width_px);
                    for (std::size_t b = 0; b < buckets; ++b) {
                        const std::size_t from = i0 + b * count / buckets;
                        const std::size_t to   = std::max(from + 1, i0 + (b + 1) * count / buckets);
                        std::size_t imin = from, imax = from;
                        for (std::size_t i = from; i < to && i <= i1; ++i) {
                            if (sample(series[si], i) < sample(series[si], imin)) { imin = i; }
                            if (sample(series[si], i) > sample(series[si], imax)) { imax = i; }
                        }
                        const f32 x = x_of(o.x_start + static_cast<f32>(from) * x_step);
                        const std::size_t first = std::min(imin, imax);
                        const std::size_t second = std::max(imin, imax);
                        plot_scratch_.push_back({x, y_of(sample(series[si], first))});
                        if (second != first) { plot_scratch_.push_back({x, y_of(sample(series[si], second))}); }
                    }
                } else {
                    for (std::size_t i = i0; i <= i1; ++i) {
                        plot_scratch_.push_back({x_of(o.x_start + static_cast<f32>(i) * x_step), y_of(sample(series[si], i))});
                    }
                }
                if (pass == 0) {
                    if (plot_scratch_.size() >= 2) { dl_.area_fill(plot_scratch_, base_y, c.scaled_alpha(o.fill_alpha), c.scaled_alpha(0.0f)); }
                } else if (plot_scratch_.size() >= 2) {
                    dl_.polyline(plot_scratch_, c, 1.6f, false);
                } else if (plot_scratch_.size() == 1) {
                    dl_.circle_filled(plot_scratch_[0], 2.5f, c);
                }
            } else if (pass == 1) {
                const f32 bw_full = x_step * kx;
                const bool bucketed = static_cast<f32>(count) > width_px;
                const std::size_t buckets = bucketed ? static_cast<std::size_t>(width_px) : count;
                const f32 bw  = bucketed ? inner.width() / static_cast<f32>(buckets) : bw_full;
                const f32 sub = series.size() > 1 ? bw / static_cast<f32>(series.size()) : bw;
                for (std::size_t b = 0; b < buckets; ++b) {
                    const std::size_t from = bucketed ? i0 + b * count / buckets : i0 + b;
                    const std::size_t to   = bucketed ? std::max(from + 1, i0 + (b + 1) * count / buckets) : from + 1;
                    f32 v = sample(series[si], from);
                    for (std::size_t i = from; i < to && i <= i1; ++i) { v = std::max(v, sample(series[si], i)); }
                    const f32 xs = bucketed ? inner.min.x + bw * static_cast<f32>(b) : x_of(o.x_start + static_cast<f32>(from) * x_step);
                    const f32 x0 = xs + sub * static_cast<f32>(si);
                    const f32 gap = sub > 3.0f ? 1.0f : 0.0f;
                    const f32 y = y_of(v);
                    dl_.rect_filled({{x0, std::min(y, base_y)}, {x0 + sub - gap, std::max(std::max(y, base_y), std::min(y, base_y) + 1.0f)}}, c, 1.0f);
                }
            }
        }
    }
    dl_.pop_clip();

    if (view.x_set || view.y_set) {
        const std::string_view hint = "zoomed - double-click to reset";
        const f32 tw = font_.measure(f, hint).x;
        dl_.text({inner.max.x - tw - 6.0f, inner.min.y + 4.0f}, style_.accent.scaled_alpha(0.85f), hint, f);
    }

    if (in.hovered && n > 0 && inner.contains(mouse_) && !(in.held && o.zoom_pan)) {
        const f32 xm = xv0 + (mouse_.x - inner.min.x) / kx;
        const std::size_t idx = static_cast<std::size_t>(std::clamp(std::round((xm - o.x_start) / x_step - (hist ? 0.5f : 0.0f)), 0.0f, static_cast<f32>(n - 1)));
        const f32 sx = x_of(o.x_start + static_cast<f32>(idx) * x_step + (hist ? 0.5f * x_step : 0.0f));
        if (sx >= inner.min.x && sx <= inner.max.x) {
            dl_.rect_filled({{std::round(sx), inner.min.y}, {std::round(sx) + 1.0f, inner.max.y}}, color{255, 255, 255, 90});
            const f32 xval = o.x_start + static_cast<f32>(idx) * x_step;
            const f32 xs_step = std::max((xv1 - xv0) / 50.0f, x_step * 0.01f);
            const f32 ys_step = (yv1 - yv0) / 50.0f;
            std::string tip = (o.x.title.empty() ? std::string{"x"} : std::string{o.x.title}) + " = " + tick_text(xval, std::min(xs_step, x_step), o.x.unit);
            for (std::size_t si = 0; si < series.size(); ++si) {
                const f32 v = sample(series[si], idx);
                if (!hist) { dl_.circle_filled({sx, y_of(v)}, 3.5f, series_color(si, series[si].col, style_)); }
                tip += '\n';
                if (!series[si].name.empty()) { tip.append(series[si].name); tip += ": "; }
                tip += tick_text(v, ys_step, o.y.unit);
            }
            draw_tooltip_at(mouse_ + vec2{14.0f, 20.0f}, tip);
        }
    }

    view.sx_lo = xv0; view.sx_hi = xv1;
    view.sy_lo = yv0; view.sy_hi = yv1;
    view.inner = inner;
}

void context::plot(std::string_view label, std::span<const plot_series> series, const plot_options& options)
{
    chart_impl(label, series, options);
}

void context::plot_lines(std::string_view label, std::span<const f32> values, vec2 size, std::string_view overlay, f32 lo, f32 hi,
                         u32 offset)
{
    const plot_series s{{}, values, color{0, 0, 0, 0}};
    plot_impl(label, {&s, 1}, size, plot_kind::lines, lo, hi, offset, overlay, false);
}

void context::plot_histogram(std::string_view label, std::span<const f32> values, vec2 size, std::string_view overlay, f32 lo, f32 hi,
                             u32 offset)
{
    const plot_series s{{}, values, color{0, 0, 0, 0}};
    plot_impl(label, {&s, 1}, size, plot_kind::histogram, lo, hi, offset, overlay, false);
}

void context::sparkline(std::span<const f32> values, vec2 size, color c, u32 offset)
{
    const plot_series s{{}, values, c};
    plot_impl("##spark", {&s, 1}, size, plot_kind::lines, plot_auto, plot_auto, offset, {}, true);
}

void context::plot(std::string_view label, std::span<const plot_series> series, vec2 size, plot_kind kind, f32 lo, f32 hi, u32 offset)
{
    plot_impl(label, series, size, kind, lo, hi, offset, {}, false);
}

} // namespace strata
