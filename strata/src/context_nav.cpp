// keyboard navigation of a list or tree, and the scroll control the rows need to keep the cursor in view.
//
// a nav scope collects every row submitted inside it -- culled ones too, since the cursor has to be able to walk
// past what is off-screen -- and resolves the keys once, at nav_end(), when the whole sequence is known. moving the
// cursor therefore takes effect on the next frame, which is also when the row draws its focus ring and reports the
// Enter as a press: one frame of lag that no one can see, in exchange for not needing a second pass.

#include "strata/context.hpp"

#include "limits.hpp"

#include <algorithm>

namespace strata {

void context::push_id_value(id key) noexcept
{
    if (id_depth_ < max_id_depth) {
        id_stack_[++id_depth_] = key;
    } else {
        internal::limit_reached("push_id nesting (max_id_depth): ids will collide", max_id_depth);
    }
}

// navigation ----------------------------------------------------------------------------------

void context::nav_begin(std::string_view id_label)
{
    const id key = hash_id(id_label, current_seed());
    if (nav_.scope != 0) {
        internal::limit_reached("nav_begin inside another nav scope (only one at a time)", 1);
        return;
    }
    if (nav_.scope_key != key) { // another list: start its cursor over
        nav_.scope_key = key;
        nav_.cursor    = 0;
    }
    nav_.scope = key;
    nav_.items.clear();
    // a text field with the keyboard uses the arrow keys itself
    nav_.active = focus_id_ == 0 && hotkey_capture_ == 0;
}

void context::nav_record(id key, const rect& r, u32 depth, bool node, bool open) noexcept
{
    if (nav_.scope == 0 || key == 0) {
        return;
    }
    nav_.items.push_back({key, r, depth, node, open});
}

bool context::nav_is_cursor(id key) const noexcept
{
    return nav_.scope != 0 && key != 0 && nav_.cursor == key;
}

// the row the cursor was on when Enter was pressed last frame reports it as a press, once
bool context::nav_take(id key) noexcept
{
    if (nav_.scope == 0 || key == 0 || nav_.activate_pending != key) {
        return false;
    }
    nav_.activate_pending = 0;
    return true;
}

void context::nav_click(id key) noexcept
{
    if (nav_.scope != 0) {
        nav_.cursor = key;
    }
}

void context::nav_end()
{
    if (nav_.scope == 0) {
        return;
    }
    const auto& items = nav_.items;
    if (items.empty()) {
        nav_.scope = 0;
        return;
    }

    // where the cursor is in this frame's sequence; a cursor whose row is gone starts at the top again
    std::size_t at    = 0;
    bool        found = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].key == nav_.cursor) {
            at    = i;
            found = true;
            break;
        }
    }

    bool moved    = false;
    bool activate = false;
    if (nav_.active) {
        const auto step = [&](std::ptrdiff_t d) {
            const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(items.size());
            at    = static_cast<std::size_t>(std::clamp(static_cast<std::ptrdiff_t>(at) + d, std::ptrdiff_t{0}, n - 1));
            moved = true;
        };
        for (u32 k = 0; k < key_count_; ++k) {
            const key_event& ev = keys_[k];
            if (ev.ctrl) { continue; }
            switch (ev.k) {
            case key::down:
                if (found) { step(1); } else { at = 0; moved = true; found = true; }
                break;
            case key::up:
                if (found) { step(-1); } else { at = items.size() - 1; moved = true; found = true; }
                break;
            case key::page_down: step(10); break;
            case key::page_up:   step(-10); break;
            case key::home:      at = 0; moved = true; found = true; break;
            case key::end:       at = items.size() - 1; moved = true; found = true; break;
            case key::enter:     if (found) { activate = true; } break;
            case key::right:
                if (found && items[at].node) {
                    if (!items[at].open) { tree_open_set(items[at].key, true); moved = true; }
                    else                 { step(1); }
                }
                break;
            case key::left:
                if (found && items[at].node && items[at].open) {
                    tree_open_set(items[at].key, false);
                    moved = true;
                } else if (found && items[at].depth > 0) {
                    // step out: the nearest row above that is one level shallower
                    const u32 want = items[at].depth - 1;
                    for (std::size_t i = at; i-- > 0;) {
                        if (items[i].depth == want) { at = i; moved = true; break; }
                    }
                }
                break;
            default: break;
            }
        }
    }

    if (moved || (activate && found)) {
        nav_.cursor = items[at].key;
    }
    if (activate) {
        nav_.activate_pending = nav_.cursor;
    }
    if (moved) { // keep the row the cursor moved to in view
        scroll_reveal_rect(items[at].bounds, false);
    }
    nav_.scope = 0;
}

// scrolling -----------------------------------------------------------------------------------

// the scroll offset and the visible rectangle of the innermost region that scrolls: the child region being built,
// otherwise the window. nullptr when neither scrolls.
f32* context::scroll_slot(rect& view) noexcept
{
    if (child_depth_ > 0) {
        child_frame& cf = child_stack_[child_depth_ - 1];
        if (cf.state == nullptr) { return nullptr; }
        view = cf.inner;
        return &cf.state->scroll;
    }
    if (cur_ == nullptr) {
        return nullptr;
    }
    const f32 shown_h = cur_->height > 0.0f ? cur_->height : cur_->capped_h;
    if (shown_h <= 0.0f) {
        return nullptr;
    }
    const f32 pad = cur_->menubar ? 0.0f : style_.padding;
    view = {{cur_->pos.x, cur_->pos.y + cur_->title_h + pad}, {cur_->pos.x + cur_->width, cur_->pos.y + shown_h - pad}};
    return &cur_->scroll;
}

f32 context::scroll_y() const noexcept
{
    context& self = *const_cast<context*>(this); // the lookup only reads; the slot it finds is what is mutable
    rect     view;
    const f32* scroll = self.scroll_slot(view);
    return scroll != nullptr ? *scroll : 0.0f;
}

f32 context::scroll_max_y() const noexcept
{
    context& self = *const_cast<context*>(this);
    rect     view;
    if (self.scroll_slot(view) == nullptr) {
        return 0.0f;
    }
    // what the region reported at the end of the last frame: this frame's content is not finished yet
    const f32 content = child_depth_ > 0 ? child_stack_[child_depth_ - 1].state->content_h
                                         : cur_->content_h + 2.0f * (cur_->menubar ? 0.0f : style_.padding);
    return std::max(0.0f, content - view.height());
}

void context::set_scroll_y(f32 y) noexcept
{
    rect view;
    if (f32* scroll = scroll_slot(view); scroll != nullptr) {
        *scroll = std::max(0.0f, y); // the owner clamps against this frame's content height
    }
}

// scrolls the least it has to (or centres, with `center`). the content of this frame is already laid out, so the new
// offset is what the next frame draws with -- which is why "reveal the selection" is called every frame it holds
void context::scroll_reveal_rect(const rect& item, bool center) noexcept
{
    rect view;
    f32* scroll = scroll_slot(view);
    if (scroll == nullptr || view.height() <= 0.0f) {
        return;
    }
    f32 delta = 0.0f;
    if (center) {
        delta = item.center().y - view.center().y;
    } else if (item.min.y < view.min.y) {
        delta = item.min.y - view.min.y;
    } else if (item.max.y > view.max.y) {
        delta = item.max.y - view.max.y;
    }
    if (delta != 0.0f) {
        *scroll = std::max(0.0f, *scroll + delta);
    }
}

void context::ensure_item_visible() noexcept
{
    scroll_reveal_rect(last_item_rect_, false);
}

void context::scroll_to_item() noexcept
{
    scroll_reveal_rect(last_item_rect_, true);
}

} // namespace strata
