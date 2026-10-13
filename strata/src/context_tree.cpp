// trees, selectable rows, list selection

#include "strata/context.hpp"

#include "context_impl.hpp"
#include "context_helpers.hpp"
#include "core/part_id.hpp"
#include "text_util.hpp"
#include "widget_util.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <string>

namespace strata {

using namespace text;
using namespace internal;

// tree open state, with set_next_item_open / bulk open-close applied
[[nodiscard]] static bool& tree_open_resolved(context& ui, id key, bool default_open, bool& recursive_out) noexcept;
static void tree_set_recursive(context& ui, id key, bool open) noexcept;
static void tree_set_bulk(context& ui, id seed, bool open) noexcept;
[[nodiscard]] static id tree_seed_of(const context& ui, id key) noexcept;
[[nodiscard]] static bool tree_under(const context& ui, id node, id root) noexcept;
[[nodiscard]] static bool id_in_scope(const context& ui, id seed) noexcept;
static bool row_item(context& ui, id key, std::string_view shown, bool selected, f32 text_indent, f32 row_height);

tree_scope::~tree_scope()
{
    if (open_) {
        ctx_->tree_pop();
    }
}

bool& internal::tree_open_state(context& ui, id key, bool default_open)
{
    auto& m = context_access::of(ui);
    auto it = std::lower_bound(m.tree_table_.tree_states_.begin(), m.tree_table_.tree_states_.end(), key,
                               [](const tree_state& s, id k) { return s.key < k; });
    if (it == m.tree_table_.tree_states_.end() || it->key != key) {
        // nodes appearing during an open-all start open, so a whole tree expands without per-level driving
        bool start_open = default_open;
        if (m.tree_bulk_ != 0 && id_in_scope(ui, m.tree_bulk_seed_)) {
            start_open = m.tree_bulk_ == 1;
        }
        it = m.tree_table_.tree_states_.insert(it, tree_state{key, current_seed(ui), start_open});
    } else {
        it->seed = current_seed(ui);
    }
    return it->open;
}

// is `seed` one of the id scopes enclosing the current item? 0 is the root: always true
static bool id_in_scope(const context& ui, id seed) noexcept
{
    auto& m = context_access::of(ui);
    if (seed == 0) {
        return true;
    }
    return m.ids_.contains(seed);
}

// the id scope a node was submitted in, 0 when it has no state
// sets a node's open state without touching its scope (the keyboard acts from outside it, so tree_open_state
// would record the wrong parent)
void internal::tree_open_set(context& ui, id key, bool open) noexcept
{
    auto& m = context_access::of(ui);
    const auto it = std::lower_bound(m.tree_table_.tree_states_.begin(), m.tree_table_.tree_states_.end(), key,
                                     [](const tree_state& s, id k) { return s.key < k; });
    if (it != m.tree_table_.tree_states_.end() && it->key == key) {
        it->open = open;
    }
}

static id tree_seed_of(const context& ui, id key) noexcept
{
    auto& m = context_access::of(ui);
    const auto it = std::lower_bound(m.tree_table_.tree_states_.begin(), m.tree_table_.tree_states_.end(), key,
                                     [](const tree_state& s, id k) { return s.key < k; });
    return it != m.tree_table_.tree_states_.end() && it->key == key ? it->seed : id{0};
}

// does the chain of id scopes above `node` reach `root`? (`root` 0 is the whole ui)
static bool tree_under(const context& ui, id node, id root) noexcept
{
    if (root == 0) {
        return true;
    }
    id at = tree_seed_of(ui, node);
    for (u32 i = 0; i < max_tree_depth && at != 0; ++i) {
        if (at == root) { return true; }
        at = tree_seed_of(ui, at);
    }
    return false;
}

// open / close every node under `seed`: existing ones by walking scopes, later ones via the pending request, which
// lives long enough for a maximum-depth tree to unfold
static void tree_set_bulk(context& ui, id seed, bool open) noexcept
{
    auto& m = context_access::of(ui);
    for (tree_state& st : m.tree_table_.tree_states_) {
        if (st.key != seed && tree_under(ui, st.key, seed)) { st.open = open; }
    }
    m.tree_bulk_        = open ? u8{1} : u8{2};
    m.tree_bulk_seed_   = seed;
    // opening unfolds a level per frame, so the request must outlive the deepest tree; closing needs only this frame
    m.tree_bulk_frames_ = open ? max_tree_depth : 1;
}

static void tree_set_recursive(context& ui, id key, bool open) noexcept
{
    tree_set_bulk(ui, key, open);
}

void context::open_all_tree_nodes() noexcept { tree_set_bulk(*this, current_seed(*this), true); }
void context::close_all_tree_nodes() noexcept { tree_set_bulk(*this, current_seed(*this), false); }

// set_next_item_open() and bulk / recursive requests, applied over the stored state
static bool& tree_open_resolved(context& ui, id key, bool default_open, bool& recursive_out) noexcept
{
    auto& m = context_access::of(ui);
    const u8 next = m.next_open_;
    m.next_open_    = 0;
    recursive_out = next >= 3;

    // existing nodes were set by tree_set_bulk's walk; the pending request catches nodes created later
    // (tree_open_state applies it)
    bool& open = tree_open_state(ui, key, default_open);
    if (next != 0) {
        open = next == 1 || next == 3;
        if (recursive_out) { tree_set_recursive(ui, key, open); }
    }
    return open;
}

// a full-width clickable row with hover / selected highlight; the caller draws the content
static bool row_item(context& ui, id key, std::string_view shown, bool selected, f32 text_indent, f32 row_height)
{
    auto& m = context_access::of(ui);
    const font_id f = ui.current_font();
    const f32 gutter = std::exchange(m.accessory_.next_gutter_, 0.0f); // room set_next_item_gutter() reserved for accessories
    const rect row = layout_place(ui, {m.layout_.width, row_height});
    // in a table cell the highlight covers the cell padding so text aligns with neighbours
    const rect hit = m.tree_table_.table_.active ? rect{{row.min.x - m.tree_table_.table_.pad_x + 2.0f, row.min.y - 2.0f}, {row.max.x + m.tree_table_.table_.pad_x - 2.0f, row.max.y + 2.0f}}
                                   : row;
    // a row that is scrolled out of view costs nothing beyond the place it takes in the layout
    note_row_anchor(ui, key, hit);
    if (item_culled(ui, hit)) {
        note_culled_item(ui, key, hit);
        nav_record(ui, key, hit, m.tree_table_.tree_depth_, false, false);
        const bool taken = nav_take(ui, key);
        m.tree_table_.item_pressed_    = taken;
        return taken;
    }
    const interaction in = interact(ui, key, hit);
    const bool activated = in.pressed || nav_take(ui, key);
    m.tree_table_.item_pressed_ = activated;
    if (in.pressed) { nav_click(ui, key); }
    nav_record(ui, key, hit, m.tree_table_.tree_depth_, false, false);

    anim_slot* a = anim_find(ui, key);
    if (a == nullptr && in.hovered) { a = &anim_for(ui, key); }
    f32 hover = 0.0f;
    if (a != nullptr) {
        a->hover = approach(ui, a->hover, in.hovered ? 1.0f : 0.0f);
        hover    = a->hover;
        a->last_frame = (hover == 0.0f && !in.hovered) ? 0 : m.frame_;
    }

    const bool focused = m.last_item_focused_;
    if (selected || focused || hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m.style_.rounding * 0.55f);
        bg.fill_top    = selected ? m.style_.accent.scaled_alpha(0.28f + 0.1f * hover)
                                  : m.style_.widget_hover.scaled_alpha(0.75f * hover);
        bg.fill_bottom = bg.fill_top;
        if (focused) { // keyboard cursor: an outline, visible on selected / hovered rows
            bg.border       = m.style_.accent_hover;
            bg.border_width = std::max(m.style_.border_width, 1.0f);
        }
        m.dl_.shape(hit, bg);
    }
    const f32   indent = m.tree_table_.table_.active ? 0.0f : text_indent;
    const vec2  tsize  = label_size(ui, f, shown);
    const vec2  at{row.min.x + indent, row.min.y + (row.height() - tsize.y) * 0.5f};
    const color col = selected ? m.style_.accent_hover : m.style_.text;
    // the label stops before the row's accessory gutter; item_truncated() reports a cut
    ui.label_clipped(at, std::max(row.max.x - gutter - at.x, 0.0f), col, shown, f);
    return activated;
}

bool context::selectable(std::string_view label, bool selected)
{
    return selectable(label, {}, selected);
}

bool context::selectable(std::string_view label, std::string_view id_extra, bool selected)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const std::string_view shown = visible_label(label);
    const f32 lh  = m_->rich_.rich_depth_ > 0 ? label_size(*this, f, shown).y : m_->font_.line_height(f);
    const id  key = id_extra.empty() ? widget_id(*this, label) : hash_id(id_extra, widget_id(*this, label));
    return row_item(*this, key, shown, selected, 8.0f, m_->tree_table_.table_.active ? lh : lh + 8.0f);
}

bool context::tree_leaf(std::string_view label, bool selected)
{
    return tree_leaf(label, {}, selected);
}

bool context::tree_leaf(std::string_view label, std::string_view id_extra, bool selected)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const std::string_view shown = visible_label(label);
    const f32 h   = (m_->rich_.rich_depth_ > 0 ? label_size(*this, f, shown).y : m_->font_.line_height(f)) + 8.0f;
    const id  key = id_extra.empty() ? widget_id(*this, label) : hash_id(id_extra, widget_id(*this, label));
    return row_item(*this, key, shown, selected, 22.0f, h);
}

bool context::tree_node(std::string_view label, tree_flags flags)
{
    return tree_node(label, {}, flags);
}

bool context::tree_node(std::string_view label, std::string_view id_extra, tree_flags flags)
{
    if (m_->cur_ != nullptr && m_->tree_table_.tree_depth_ >= max_tree_depth) {
        report_limit(*this, "tree_node nesting (max_tree_depth)", max_tree_depth);
    }
    if (m_->cur_ == nullptr || m_->tree_table_.tree_depth_ >= max_tree_depth) {
        return false;
    }
    const font_id f = current_font();
    const id key    = id_extra.empty() ? widget_id(*this, label) : hash_id(id_extra, widget_id(*this, label));
    const std::string_view shown = visible_label(label);
    const f32 h     = (m_->rich_.rich_depth_ > 0 ? label_size(*this, f, shown).y : m_->font_.line_height(f)) + 8.0f;

    const f32 gutter = std::exchange(m_->accessory_.next_gutter_, 0.0f);
    const rect row = layout_place(*this, {m_->layout_.width, h});
    bool  recursive = false;
    bool& open      = tree_open_resolved(*this, key, has_flag(flags, tree_flags::default_open), recursive);

    constexpr f32 indent = 18.0f;
    // a culled node keeps its open state and layout slot; only hit test, animation, measuring and geometry are skipped
    note_row_anchor(*this, key, row);
    if (item_culled(*this, row)) {
        note_culled_item(*this, key, row);
        nav_record(*this, key, row, m_->tree_table_.tree_depth_, true, open);
        (void)nav_take(*this, key);
        if (!open) {
            return false;
        }
        m_->tree_table_.tree_stack_[m_->tree_table_.tree_depth_++] = {row.min.x + 10.0f, row.max.y, m_->layout_.origin.x, m_->layout_.width};
        m_->layout_.origin.x += indent;
        m_->layout_.width    -= indent;
        push_id_value(*this, key);
        return true;
    }

    const interaction in = interact(*this, key, row);

    const bool arrow_hit = m_->input_.mouse_.x < row.min.x + h;
    const bool activated = in.pressed || nav_take(*this, key);
    m_->tree_table_.item_pressed_    = activated;
    m_->last_item_arrow_ = in.pressed && arrow_hit;
    if (in.pressed) { nav_click(*this, key); }
    if (activated && (!has_flag(flags, tree_flags::arrow_only) || arrow_hit)) {
        open = !open;
        // Ctrl or Shift held while it is clicked applies the change to the whole subtree
        if (in.pressed && (m_->input_.mod_ctrl_ || m_->input_.mod_shift_)) { tree_set_recursive(*this, key, open); }
    }
    const bool is_open = open;
    nav_record(*this, key, row, m_->tree_table_.tree_depth_, true, is_open);

    anim_slot* slot = anim_find(*this, key);
    if (slot == nullptr) {
        slot         = &anim_for(*this, key);
        slot->toggle = is_open ? 1.0f : 0.0f; // a node scrolling back into view keeps the arrow it had
    }
    anim_slot& a = *slot;
    a.hover  = approach(*this, a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(*this, a.toggle, is_open ? 1.0f : 0.0f, m_->style_.anim_speed * 0.9f);

    const bool selected = has_flag(flags, tree_flags::selected);
    const bool focused  = m_->last_item_focused_;
    if (selected || focused || a.hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m_->style_.rounding * 0.55f);
        bg.fill_top    = selected ? m_->style_.accent.scaled_alpha(0.28f + 0.1f * a.hover)
                                  : m_->style_.widget_hover.scaled_alpha(0.75f * a.hover);
        bg.fill_bottom = bg.fill_top;
        if (focused) {
            bg.border       = m_->style_.accent_hover;
            bg.border_width = std::max(m_->style_.border_width, 1.0f);
        }
        m_->dl_.shape(row, bg);
    }

    // arrow that rotates from "right" to "down" while opening
    const vec2 c{row.min.x + 10.0f, row.center().y};
    const f32  ang = a.toggle * std::numbers::pi_v<f32> * 0.5f;
    const f32  cs = std::cos(ang);
    const f32  sn = std::sin(ang);
    const auto rot = [&](vec2 p) { return vec2{c.x + p.x * cs - p.y * sn, c.y + p.x * sn + p.y * cs}; };
    m_->dl_.triangle_filled(rot({-2.5f, -4.0f}), rot({4.0f, 0.0f}), rot({-2.5f, 4.0f}),
                        lerp(m_->style_.text_dim, m_->style_.text, std::max(a.hover, a.toggle)));

    const vec2  tsize = label_size(*this, f, shown);
    const vec2  at{row.min.x + 22.0f, row.min.y + (row.height() - tsize.y) * 0.5f};
    const color col = selected ? m_->style_.accent_hover : m_->style_.text;
    label_clipped(at, std::max(row.max.x - gutter - at.x, 0.0f), col, shown, f);

    if (!is_open) {
        return false;
    }

    m_->tree_table_.tree_stack_[m_->tree_table_.tree_depth_++] = {c.x, row.max.y, m_->layout_.origin.x, m_->layout_.width};
    m_->layout_.origin.x += indent;
    m_->layout_.width    -= indent;
    push_id_value(*this, key);
    return true;
}

void context::tree_pop()
{
    if (m_->tree_table_.tree_depth_ == 0) {
        return;
    }
    const tree_frame fr = m_->tree_table_.tree_stack_[--m_->tree_table_.tree_depth_];
    pop_id();

    const f32 y0 = fr.children_top + 1.0f;
    const f32 y1 = m_->layout_.bottom - 2.0f;
    if (y1 > y0) {
        m_->dl_.rect_filled({{std::round(fr.arrow_x), y0}, {std::round(fr.arrow_x) + 1.0f, y1}}, m_->style_.border.scaled_alpha(0.8f));
    }
    m_->layout_.origin.x = fr.saved_origin_x;
    m_->layout_.width    = fr.saved_width;
}

void context::text_ellipsis(std::string_view s)
{
    if (m_->cur_ == nullptr) {
        return;
    }
    const font_id f = current_font();
    const f32 avail = m_->layout_.width;
    if (m_->rich_.rich_depth_ > 0) { // markup cannot be cut safely: clip it instead
        const vec2 size = label_size(*this, f, s);
        const rect r    = layout_place(*this, {std::min(size.x, avail), size.y});
        m_->dl_.push_clip(r);
        label_draw(*this, r.min, m_->style_.text, s, f);
        m_->dl_.pop_clip();
        return;
    }
    if (measure_cached(*this, f, s).x <= avail) {
        text(s);
        return;
    }
    m_->last_item_truncated_ = true; // the caller can add a tooltip with the whole string

    constexpr std::string_view dots = "...";
    const f32 budget = avail - m_->font_.measure(f, dots).x;
    std::string cut;
    f32 used = 0.0f;
    std::string_view rest = s;
    while (!rest.empty()) {
        const std::string_view before = rest;
        const char32_t cp = decode_utf8(rest);
        const f32 adv = m_->font_.advance(f, cp);
        if (used + adv > budget) { break; }
        used += adv;
        cut.append(before.substr(0, before.size() - rest.size()));
    }
    cut += dots;
    text(cut);
    m_->last_item_truncated_ = true;
}

bool context::selection_click(selection_state& sel, int index) const
{
    if (m_->input_.mod_shift_ && sel.anchor() >= 0) {
        const int from = sel.anchor();
        if (!m_->input_.mod_ctrl_) { sel.clear(); }
        sel.add_range(std::min(from, index), std::max(from, index));
        sel.set_anchor(from); // the range keeps growing from where it started
        return true;
    }
    if (m_->input_.mod_ctrl_) {
        sel.toggle(index);
        return true;
    }
    if (sel.size() == 1 && sel.contains(index)) {
        return false; // already the only one selected
    }
    sel.select_one(index);
    return true;
}

} // namespace strata
