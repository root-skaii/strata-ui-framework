#pragma once

// internal: the widget inspector's record of one frame, behind context::impl::inspect_. only filled while the inspector
// window is open (it asks for recording one frame ahead), so a closed inspector costs one branch per widget.

#include "strata/types.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace strata::internal {

struct inspected_item {
    id                    key{};
    rect                  box;     // clipped to the visible region
    id                    window{};
    std::array<char, 40>  label{};
    u8                    label_len{};
    u8                    depth{};   // id scopes (push_id / windows) the widget sits in
    u8                    flags{};   // bit 0 hovered, 1 held, 2 pressed
    u8                    color_overrides{}; // style overrides in effect (push_color / push_var) when it was submitted
    u8                    var_overrides{};

    [[nodiscard]] std::string_view name() const noexcept { return {label.data(), label_len}; }
};

struct inspector_state {
    static constexpr std::size_t max_items = 20000;

    bool want{};      // the inspector window ran this frame: record the next one
    bool recording{};
    std::vector<inspected_item> cur;
    std::vector<inspected_item> prev;
    struct label_entry {
        id                   key{};
        std::array<char, 40> text{};
        u8                   len{};
    };
    std::vector<label_entry> labels;

    bool        pick{};
    bool        swallow{};      // pick mode is on and the pointer is outside the inspector: its click is not the application's
    bool        swallow_prev{};
    id          picked{};
    std::string filter;
    rect        frame; // the inspector window, so picking ignores the pointer over it

    void begin_frame()
    {
        prev.swap(cur);
        cur.clear();
        labels.clear();
        recording = want;
        swallow_prev = swallow;
        swallow      = false;
        want      = false;
    }

    void note_label(id key, std::string_view s)
    {
        if (labels.size() >= max_items) { return; }
        label_entry e;
        e.key = key;
        e.len = static_cast<u8>(std::min(s.size(), e.text.size()));
        std::copy_n(s.data(), e.len, e.text.begin());
        labels.push_back(e);
    }

    void note_item(id key, const rect& box, id window, u8 depth, u8 flags, u8 colors, u8 vars)
    {
        if (cur.size() >= max_items) { return; }
        inspected_item it;
        it.key    = key;
        it.box    = box;
        it.window = window;
        it.depth  = depth;
        it.flags  = flags;
        it.color_overrides = colors;
        it.var_overrides   = vars;
        // the label is noted just before its widget asks for interaction, so the newest match is the one
        for (auto i = labels.rbegin(); i != labels.rend(); ++i) {
            if (i->key == key) {
                it.label     = i->text;
                it.label_len = i->len;
                break;
            }
        }
        cur.push_back(it);
    }
};

} // namespace strata::internal
