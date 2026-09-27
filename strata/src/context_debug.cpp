// the framework's own inspector windows: what stats() measured, and the draw commands the renderer is about to run.
// everything here reads state that is already collected, so a build that never calls these pays nothing for them.

#include "strata/context.hpp"

#include "context_impl.hpp"

#include <algorithm>

namespace strata {

namespace {

// "12.3 k" / "1.20 M": the counts here span four orders of magnitude and the exact digits rarely matter
[[nodiscard]] std::string big(u32 n)
{
    if (n < 10'000) { return std::format("{}", n); }
    if (n < 1'000'000) { return std::format("{:.1f} k", static_cast<f64>(n) / 1'000.0); }
    return std::format("{:.2f} M", static_cast<f64>(n) / 1'000'000.0);
}

} // namespace

void context::debug_metrics_window(bool& open)
{
    if (!open) {
        return;
    }
    const frame_stats& st = m_->stats_prev_;

    // its own id scope: the labels in here are common words ("vertices", "frame") and would otherwise be a
    // collision report of their own the moment an application uses the same ones
    push_id("##strata_metrics");
    if (auto w = window("strata metrics", {40.0f, 40.0f}, {330.0f, 0.0f},
                        window_flags::resizable | window_flags::drag_by_body)) {
        const auto row = [&](std::string_view name, std::string_view value) {
            text_dim(name);
            same_line_right(label_size(current_font(), value).x);
            text(value);
        };
        const auto row_num = [&](std::string_view name, u32 value) { row(name, big(value)); };

        const f64 ui_ms = st.begin_frame_ms + st.end_frame_ms;
        row("ui cpu", std::format("{:.3f} ms", ui_ms));
        row("  begin / end", std::format("{:.3f} / {:.3f}", st.begin_frame_ms, st.end_frame_ms));
        separator();

        row_num("vertices", st.vertices);
        row_num("indices", st.indices);
        row_num("draw calls", st.draw_calls);
        // 16 bytes a vertex, 2 an index: what the frame costs to hand to the gpu, which is the number the idle
        // path below is trying to avoid paying
        row("upload", std::format("{:.1f} KiB", static_cast<f64>(st.vertices * 16 + st.indices * 2) / 1024.0));
        separator();

        row_num("items submitted", st.items_submitted);
        if (st.items_submitted > 0) {
            const f64 pct = 100.0 * static_cast<f64>(st.items_culled) / static_cast<f64>(st.items_submitted);
            row("  culled", std::format("{} ({:.0f} %)", big(st.items_culled), pct));
        }
        const u32 measures = st.text_measures + st.measure_hits;
        if (measures > 0) {
            const f64 pct = 100.0 * static_cast<f64>(st.measure_hits) / static_cast<f64>(measures);
            row("measure cache", std::format("{:.0f} % of {}", pct, big(measures)));
        }
        row("anim slots", std::format("{} / {}", st.anim_slots_used, st.anim_slots_total));
        separator();

        // idling: the two predicates a host needs, and what they add up to
        row("geometry", st.unchanged ? "unchanged" : "changed");
        row("animating", st.animating ? "yes" : "no");
        {
            const bool idle = st.unchanged && !st.animating;
            text_dim("can idle");
            const std::string_view v = idle ? "yes" : "no";
            same_line_right(label_size(current_font(), v).x);
            text_colored(idle ? kind_color(toast_kind::success, m_->style_) : m_->style_.text, v);
        }

        // the quiet failures. nothing is shown while there are none, so a clean window stays short and anything
        // appearing here is worth reading.
        const bool trouble = st.draw_overflow != 0 || st.clip_overflows != 0 || st.alpha_overflows != 0 ||
                             st.id_collisions != 0;
        if (trouble) {
            separator();
            const color bad = kind_color(toast_kind::error, m_->style_);
            if (st.draw_overflow != 0) {
                text_colored(bad, "draw list overflowed: geometry was dropped");
                text_dim("raise draw_list_limits (context_config)");
            }
            if (st.clip_overflows != 0) {
                text_colored(bad, std::format("clip stack overflowed {}x", st.clip_overflows));
            }
            if (st.alpha_overflows != 0) {
                text_colored(bad, std::format("alpha stack overflowed {}x", st.alpha_overflows));
            }
            if (st.id_collisions != 0) {
                text_colored(bad, std::format("{} duplicate widget id{}", st.id_collisions,
                                              st.id_collisions == 1 ? "" : "s"));
                if (const std::string_view label = id_collision_label(); !label.empty()) {
                    text_dim(std::format("first: \"{}\"", label));
                } else if (id_collision() != 0) {
                    text_dim(std::format("first: id {:#010x}", id_collision()));
                }
                text_dim("give one a \"label##suffix\", or push_id()");
            }
        }
    }
    pop_id();
}

void context::debug_draw_list_window(bool& open)
{
    if (!open) {
        return;
    }
    // the commands of the frame being built right now, which is everything submitted before this window. that is
    // the honest thing to show: this window's own commands do not exist yet.
    const draw_data dd = m_->dl_.data();

    push_id("##strata_cmds");
    if (auto w = window("strata draw list", {400.0f, 40.0f}, {430.0f, 320.0f},
                        window_flags::resizable | window_flags::drag_by_body)) {
        textf("{} commands, {} indices", dd.commands.size(), dd.indices.size());
        separator();
        if (begin_table("cmds", 5, table_flags::striped | table_flags::borders | table_flags::row_hover)) {
            table_setup_column("#", 28.0f);
            table_setup_column("indices", 54.0f);
            table_setup_column("vtx", 54.0f);
            table_setup_column("tex", 34.0f);
            table_setup_column("clip");
            table_headers_row();
            const int count = static_cast<int>(dd.commands.size());
            int first = 0, last = count;
            list_clip_begin(count, frame_height(), first, last);
            for (int i = first; i < last; ++i) {
                const draw_cmd& c = dd.commands[static_cast<std::size_t>(i)];
                push_id(i);
                if (table_next_row()) {
                    table_next_column(); textf("{}", i);
                    table_next_column(); textf("{}", c.idx_count);
                    table_next_column(); textf("{}", c.vtx_offset);
                    table_next_column();
                    if (c.blur > 0.0f)        { text_dim("blur"); }
                    else if (c.texture != 0)  { textf("{}", c.texture); }
                    else                      { text_dim("-"); }
                    table_next_column();
                    textf("{:.0f},{:.0f} {:.0f}x{:.0f}", c.clip.min.x, c.clip.min.y, c.clip.width(), c.clip.height());
                }
                pop_id();
            }
            list_clip_end(count, frame_height(), last);
            end_table();
        }
    }
    pop_id();
}

} // namespace strata
