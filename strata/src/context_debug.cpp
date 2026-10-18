// the framework's inspector windows: stats() and the pending draw commands. reads existing state only, so unused
// means free.

#include "strata/context.hpp"

#include "context_impl.hpp"
#include "context_helpers.hpp"

#include <algorithm>

namespace strata {

using namespace internal;

namespace {

// "12.3 k" / "1.20 M": the counts span four orders of magnitude and exact digits rarely matter
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

    // own id scope: common labels ("vertices", "frame") would otherwise collide with the app's
    push_id("##strata_metrics");
    if (auto w = window("strata metrics", {40.0f, 40.0f}, {330.0f, 0.0f},
                        window_flags::resizable | window_flags::drag_by_body)) {
        const auto row = [&](std::string_view name, std::string_view value) {
            text_dim(name);
            same_line_right(label_size(*this, current_font(), value).x);
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
        // 16 bytes per vertex, 2 per index: the per-frame gpu upload the idle path avoids
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
            same_line_right(label_size(*this, current_font(), v).x);
            text_colored(idle ? kind_color(toast_kind::success, m_->style_) : m_->style_.text, v);
        }

        // silent failures; hidden while there are none, so anything here is worth reading.
        const bool trouble = st.draw_overflow != 0 || st.clip_overflows != 0 || st.alpha_overflows != 0 ||
                             st.id_collisions != 0 || st.limits_hit != 0;
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
            if (st.limits_hit != 0) {
                text_colored(bad, std::format("{} fixed limit{} hit", st.limits_hit, st.limits_hit == 1 ? "" : "s"));
                text_dim("see the diagnostics hook (or stderr); raise context_config::capacity");
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
    // commands of the frame being built (everything before this window; its own do not exist yet).
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

namespace {

[[nodiscard]] bool contains_nocase(std::string_view hay, std::string_view needle) noexcept
{
    if (needle.empty()) { return true; }
    const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); };
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != hay.end();
}

[[nodiscard]] std::string window_title(const context& ui, id win)
{
    if (win == 0) { return "(outside windows)"; }
    if (const window_state* w = window_find(ui, win); w != nullptr && w->title_len != 0) {
        return std::string{w->title.data(), w->title_len};
    }
    return std::format("window {:#x}", win);
}

} // namespace

void context::debug_inspector_window(bool& open)
{
    if (!open) {
        return;
    }
    auto& ins = m_->inspect_;
    ins.want = true;
    // the inspector's own widgets stay out of the record it shows
    const bool was_recording = ins.recording;
    ins.recording            = false;

    const vec2 mouse = mouse_pos();
    const inspected_item* hovered = nullptr;
    ins.swallow = ins.pick && !ins.frame.contains(mouse);
    if (ins.swallow) {
        for (auto it = ins.prev.rbegin(); it != ins.prev.rend(); ++it) {
            if (it->box.contains(mouse)) { hovered = &*it; break; }
        }
        if (hovered != nullptr && mouse_clicked()) { ins.picked = hovered->key; }
    }
    const inspected_item* picked = nullptr;
    if (ins.picked != 0) {
        for (const inspected_item& it : ins.prev) {
            if (it.key == ins.picked) { picked = &it; }
        }
    }
    const inspected_item* listed = nullptr; // the row under the pointer in the list

    push_id("##strata_inspector");
    if (auto w = window("strata inspector", {440.0f, 40.0f}, {430.0f, 420.0f},
                        window_flags::resizable | window_flags::drag_by_body)) {
        ins.frame = m_->cur_frame_;
        checkbox("pick (outline the item under the pointer, click to select)", ins.pick);
        input_text("filter", ins.filter);
        textf("{} items recorded", ins.prev.size());

        const inspected_item* info = hovered != nullptr ? hovered : picked;
        if (info != nullptr) {
            separator();
            text(info->label_len != 0 ? info->name() : std::string_view{"(no label)"});
            text_dim(std::format("id {:#018x}", info->key));
            text_dim(std::format("rect {:.0f},{:.0f}  {:.0f} x {:.0f}", info->box.min.x, info->box.min.y, info->box.width(),
                                 info->box.height()));
            text_dim(std::format("in {}, {} id scope{}", window_title(*this, info->window), info->depth, info->depth == 1 ? "" : "s"));
            text_dim(info->flags == 0 ? "idle"
                                      : std::format("{}{}{}", (info->flags & 1) != 0 ? "hovered " : "", (info->flags & 2) != 0 ? "held " : "",
                                                    (info->flags & 4) != 0 ? "pressed" : ""));
        }
        separator();

        // grouped by window, in the order the windows were first seen
        std::vector<id> windows;
        for (const inspected_item& it : ins.prev) {
            if (std::find(windows.begin(), windows.end(), it.window) == windows.end()) { windows.push_back(it.window); }
        }
        for (const id win : windows) {
            u32 shown = 0;
            for (const inspected_item& it : ins.prev) {
                shown += it.window == win && contains_nocase(it.name(), ins.filter) ? 1u : 0u;
            }
            if (shown == 0) { continue; }
            push_id(win);
            if (tree_node(std::format("{} ({})", window_title(*this, win), shown), "window", tree_flags::none)) {
                for (u32 i = 0; i < ins.prev.size(); ++i) {
                    const inspected_item& it = ins.prev[i];
                    if (it.window != win || !contains_nocase(it.name(), ins.filter)) { continue; }
                    push_id(static_cast<u64>(i));
                    if (selectable(it.label_len != 0 ? it.name() : std::string_view{"-"}, "row", it.key == ins.picked)) {
                        ins.picked = it.key;
                    }
                    if (item_hovered()) { listed = &it; }
                    pop_id();
                }
                tree_pop();
            }
            pop_id();
        }
    }
    pop_id();
    ins.recording = was_recording;

    const auto outline = [&](const inspected_item* it, color c) {
        if (it != nullptr && !it->box.empty()) { draw().rect_outline(it->box, c, 0.0f, 2.0f); }
    };
    auto fg = layer(layer::foreground);
    outline(picked, m_->style_.warning);
    outline(hovered != nullptr ? hovered : listed, m_->style_.accent);
}

} // namespace strata
