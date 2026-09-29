#pragma once

// internal: rich-text markup (rich_text/rich_text_wrapped) layout scratch and link tracking, behind
// context::impl::rich_. context::rich_run/rich_seg/rich_line (declared in context.hpp) alias the types below,
// but they're only ever used unqualified from within context_rich.cpp, which owns the logic. rich_depth_ is
// also read elsewhere (tree/tab/menu labels) as a simple "is markup active" query via
// push_rich_labels/pop_rich_labels/rich_labels_active (context_access.cpp).

#include "strata/context.hpp"

#include <string>
#include <vector>

namespace strata::internal {

struct rich_run {
    std::string_view text;
    font_id          font{};
    color            col;
    text_flags       style{};
    std::string_view link;      // href of the enclosing <a=...>, empty if not a link
    bool             own_col{}; // a <c=> in the link set the colour: keep it
};

struct rich_seg {
    std::string_view text;
    font_id          font{};
    color            col;
    text_flags       style{};
    f32              x{};
    std::string_view link;
    bool             own_col{};
};

struct rich_line {
    u32 first{};
    u32 count{};
    f32 width{};
    f32 asc{};
    f32 height{};
    f32 y{};
};

struct rich_text_state {
    u32                     rich_depth_{};
    std::vector<rich_run>   rich_runs_;
    std::vector<rich_seg>   rich_segs_;
    std::vector<rich_line>  rich_lines_;

    // reported hrefs are copied out of the caller's markup into reused strings. `rich_links_live_` is set only
    // by rich_text / rich_text_wrapped: links in captions must not steal the widget's click.
    std::string rich_clicked_;
    std::string rich_hovered_;
    bool        rich_links_live_{};
};

} // namespace strata::internal
