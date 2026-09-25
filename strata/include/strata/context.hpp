#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/log.hpp"
#include "strata/types.hpp"

#include <array>
#include <cmath>
#include <concepts>
#include <expected>
#include <format>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <string_view>
#include <vector>

namespace strata {

// keyboard ------------------------------------------------------------------

enum class key : u8 {
    left, right, up, down, home, end, backspace, del, enter, escape, tab, page_up, page_down,
    a, c, v, x, z, y, // only reported together with ctrl
};

struct key_event {
    key  k{};
    bool ctrl{};
    bool shift{};
};

inline constexpr u32 max_key_events  = 16;
inline constexpr u32 max_typed_bytes = 64;

// mouse_pos and display_size are in physical pixels (what the os reports); the context divides them by its scale
struct input_state {
    vec2                mouse_pos{};
    std::array<bool, 3> mouse_down{};
    f32                 wheel{};
    f32                 delta_time = 1.0f / 60.0f;
    vec2                display_size{};

    // this frame's key presses (auto-repeat included) and typed text (utf-8)
    std::array<key_event, max_key_events> keys{};
    u32                                   key_count{};
    std::array<char, max_typed_bytes>     typed{};
    u32                                   typed_len{};
    // last key (virtual-key code) or extra mouse button (4 middle, 5 / 6 side) pressed this frame; 0 if none. used by hotkey()
    u32                                   pressed_key{};
    // text the IME is composing (utf-8) and its caret in bytes. a state, not an event: the same string every frame until
    // the user confirms it, which then arrives as `typed`
    std::array<char, 256>                 ime{};
    u32                                   ime_len{};
    u32                                   ime_cursor{};
    // modifier keys held right now (drag widgets use them: shift = fine, alt = coarse)
    bool                                  ctrl{};
    bool                                  shift{};
    bool                                  alt{};
};

// readable name of a windows virtual-key code ("A", "F5", "Space", "Mouse 4" ...); "None" for 0
[[nodiscard]] std::string_view key_name(u32 virtual_key) noexcept;

// text fields copy / paste through these; win32_platform::clipboard() provides them
struct clipboard_hooks {
    void (*set)(void* user, std::string_view text) noexcept = nullptr;
    bool (*get)(void* user, std::string& out) noexcept      = nullptr;
    void* user                                              = nullptr;
};

// style ---------------------------------------------------------------------

struct style {
    f32  padding      = 12.0f;
    f32  item_spacing = 7.0f;
    f32  rounding     = 8.0f;
    f32  border_width = 1.0f;
    f32  shadow_blur  = 16.0f;   // 0 disables window / knob shadows
    f32  gradient     = 0.10f;   // vertical lighten/darken applied to widget fills
    f32  anim_speed   = 12.0f;
    vec2 frame_padding{10.0f, 5.0f};
    f32  blur_radius   = 18.0f;   // acrylic panels: how far what is behind them is blurred (logical pixels)
    f32  acrylic_alpha = 0.62f;   // acrylic windows: the window_bg alpha is multiplied by this
    f32  acrylic_noise = 0.035f;  // fine grain over acrylic panels (0 .. 1)
    f32  acrylic_saturation = 1.0f; // acrylic panels: how vivid the blurred background is (0 grey, 1 as is, > 1 boosted)
    f32  acrylic_brightness = 1.0f; // ... and how bright (1 as is)
    f32  popup_acrylic = 0.0f;    // menus, dropdowns, tooltips and toasts: 0 = opaque, 1 = frosted glass like acrylic windows

    color window_bg     = color::from_hex(0x14161df4);
    color title_bg      = color::from_hex(0x1c1f2aff);
    color border        = color::from_hex(0x2e3446ff);
    color widget_bg     = color::from_hex(0x262b3aff);
    color widget_hover  = color::from_hex(0x30374bff);
    color widget_active = color::from_hex(0x3a4462ff);
    color widget_border = color::from_hex(0x363d54ff);
    color accent        = color::from_hex(0x5b8dffff);
    color accent_hover  = color::from_hex(0x7ba4ffff);
    color text          = color::from_hex(0xe9ebf1ff);
    color text_dim      = color::from_hex(0x8a91a6ff);
    color shadow        = color::from_hex(0x000000a8);
    color modal_dim     = color::from_hex(0x000000a8); // what covers everything below a modal window
};

// which style members push_color / push_var can override temporarily
enum class style_color : u8 {
    window_bg, title_bg, border, widget_bg, widget_hover, widget_active, widget_border,
    accent, accent_hover, text, text_dim, shadow, modal_dim,
    count_
};

enum class style_var : u8 {
    padding, item_spacing, rounding, border_width, shadow_blur, gradient, anim_speed,
    frame_padding_x, frame_padding_y, blur_radius, acrylic_alpha, acrylic_noise,
    acrylic_saturation, acrylic_brightness, popup_acrylic,
    count_
};

struct style_override {
    bool  is_color{};
    u8    which{};
    color c{};
    f32   v{};
};

[[nodiscard]] constexpr style_override override_color(style_color which, color c) noexcept
{
    return {true, static_cast<u8>(which), c, 0.0f};
}

[[nodiscard]] constexpr style_override override_var(style_var which, f32 v) noexcept
{
    return {false, static_cast<u8>(which), {}, v};
}

struct context_config {
    font_config                  font{};          // font 0: window titles and the default
    std::span<const font_config> extra_fonts{};   // fonts 1, 2, ... (must outlive create())
    u32                          max_atlas_size = 4096;
    strata::style                theme{};
    draw_list_limits             limits{};
};

// widget parameters ------------------------------------------------------------

// result of context::custom_item
struct item_result {
    rect bounds;
    bool hovered{};
    bool held{};
    bool pressed{};
};

enum class input_flags : u8 {
    none                = 0,
    password            = 1, // shows bullets, disables copy / cut / undo
    read_only           = 2, // selectable and copyable, not editable
    select_all_on_focus = 4,
    no_wrap             = 8, // input_multiline: do not wrap long lines, scroll sideways instead
    no_frame            = 16, // input_multiline: no background, border or padding (the text sits directly in the layout)
    auto_height         = 32, // input_multiline: as tall as its text, so it never scrolls
};

[[nodiscard]] constexpr input_flags operator|(input_flags a, input_flags b) noexcept
{
    return static_cast<input_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

template <class E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr bool has_flag(E set, E f) noexcept
{
    return (static_cast<u32>(set) & static_cast<u32>(f)) != 0;
}

enum class window_flags : u8 {
    none          = 0,
    resizable     = 1,  // drag the right edge, bottom edge or bottom-right corner
    no_title_bar  = 2,  // no title bar (and so no collapse arrow); the window can still be moved with drag_by_body
    no_collapse   = 4,  // keep the title bar but hide the collapse arrow
    no_move       = 8,  // the window cannot be dragged
    no_background = 16, // no fill, border or shadow: only the content is drawn
    drag_by_body  = 32, // dragging any empty part of the window moves it
    dockable      = 64, // can be dropped into the dock area (see dock_area()); needs a title bar to drag
    acrylic       = 128, // frosted glass: the frame behind the window, blurred, shows through its (translucent) background
};

[[nodiscard]] constexpr window_flags operator|(window_flags a, window_flags b) noexcept
{
    return static_cast<window_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

// what the pointer should look like, for hosts that can change it (win32_platform::set_cursor)
enum class cursor_kind : u8 { arrow, text, resize_ew, resize_ns, resize_nwse };

enum class child_flags : u8 {
    none         = 0,
    frame        = 1, // draw a rounded background and border
    no_padding   = 2,
    no_scrollbar = 4, // still clips and scrolls with the wheel, without the scrollbar
    acrylic      = 8, // frosted glass background (see window_flags::acrylic)
};

[[nodiscard]] constexpr child_flags operator|(child_flags a, child_flags b) noexcept
{
    return static_cast<child_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

enum class tab_strip_flags : u8 {
    none       = 0,
    icons_only = 1, // narrow strip of icons; the labels show up as tooltips
};

enum class color_flags : u8 {
    none     = 0,
    no_alpha = 1, // hide the alpha bar and keep alpha at its current value
};

enum class tree_flags : u8 {
    none         = 0,
    default_open = 1, // open the first time it is seen
    selected     = 2, // highlight the row
    arrow_only   = 4, // only the arrow toggles; read the click with item_pressed()
};

[[nodiscard]] constexpr tree_flags operator|(tree_flags a, tree_flags b) noexcept
{
    return static_cast<tree_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

enum class table_flags : u8 {
    none      = 0,
    striped   = 1, // alternate row background
    borders   = 2, // outer border, column separators, row lines
    resizable = 4, // drag the header edges to resize columns
    row_hover = 8, // highlight the row under the pointer
};

[[nodiscard]] constexpr table_flags operator|(table_flags a, table_flags b) noexcept
{
    return static_cast<table_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

inline constexpr table_flags table_default =
    table_flags::striped | table_flags::borders | table_flags::resizable | table_flags::row_hover;

// modal windows: what closes them besides the code
enum class modal_flags : u8 {
    none            = 0,
    esc_closes      = 1, // Esc closes the modal (unless a text field or a popup inside it has focus)
    backdrop_closes = 2, // a click on the dimmed area around it closes it
    no_title_bar    = 4,
    resizable       = 8,
};

[[nodiscard]] constexpr modal_flags operator|(modal_flags a, modal_flags b) noexcept
{
    return static_cast<modal_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

enum class toast_kind : u8 { info, success, warning, error };

// a toast that can be told about later (progress, closing, which button was pressed); 0 = none
using toast_handle = u64;
inline constexpr f32 toast_no_progress = -1.0f;
inline constexpr f32 toast_busy        = -2.0f; // an endless bar for work of unknown length

struct toast_options {
    std::string_view                  title;      // empty: the kind's name (or nothing for info)
    std::string_view                  text;
    toast_kind                        kind{toast_kind::info};
    f32                               seconds{3.5f}; // <= 0: stays until closed (a progress toast closes 2 s after it completes)
    std::span<const std::string_view> actions;    // buttons under the text; pressing one closes the toast (see toast_action)
    f32                               progress{toast_no_progress}; // 0..1: a bar that fills; toast_busy: an endless one
};
enum class screen_corner : u8 { top_right, top_left, bottom_right, bottom_left };

enum class log_view_flags : u8 {
    none       = 0,
    no_toolbar = 1, // just the lines
};

// automatic value range of a plot
inline constexpr f32 plot_auto = std::numeric_limits<f32>::quiet_NaN();

enum class plot_kind : u8 { lines, histogram };

struct plot_series {
    std::string_view    name;
    std::span<const f32> values;
    color               col{0, 0, 0, 0}; // alpha 0 = one of the theme's series colors (a `{}` here would be opaque black)
};

// a styled range of the contents of a text field (see context::input_spans): bytes [start, end) are drawn with another font,
// color and / or text style. the field stays plain text; you produce the spans from it (a highlighter, a markup parser)
struct text_span {
    u32        start{};
    u32        end{};
    font_id    font{0};
    color      col{0, 0, 0, 0};              // alpha 0: the field's own text color
    text_flags style{text_flags::none};
};

// one axis of a chart (see context::plot with plot_options)
struct plot_axis {
    std::string_view title;             // "time": drawn with the chart
    std::string_view unit;              // "ms": appended to every tick label
    f32              lo{plot_auto};     // a fixed start / end; plot_auto fits the data
    f32              hi{plot_auto};
};

struct plot_options {
    plot_kind kind{plot_kind::lines};
    vec2      size{0.0f, 160.0f};
    plot_axis x;
    plot_axis y;
    f32       x_start{0.0f};            // sample i is at x_start + i * x_step on the x axis
    f32       x_step{1.0f};
    u32       offset{0};                // ring buffer: index of the oldest sample
    bool      ticks{true};              // "nice" tick marks, labels and grid lines on both axes
    bool      fill{false};              // line series: the area under the line, fading toward the axis
    f32       fill_alpha{0.28f};
    bool      zoom_pan{false};          // wheel zooms x (Ctrl + wheel: y), dragging pans, double-click resets
};

// everything a menu row can have (see context::menu_item)
struct menu_item_options {
    std::string_view shortcut;            // shown on the right, e.g. "Ctrl+O" (context::accelerator() makes it work)
    std::string_view icon;                // a glyph (or any short text) in the icon column, drawn with `icon_font`
    font_id          icon_font{0};        // e.g. the icon font's id
    color            icon_color{0, 0, 0, 0}; // alpha 0: the text color
    texture_id       image{0};            // a texture instead of a glyph, drawn as a small square
    bool             selected{false};     // a check mark (with an icon: the icon is highlighted)
    bool             enabled{true};
    bool             keep_open{false};    // clicking does not close the menus (toggles, tool options)
};

// where a window goes when docked: `center` joins the tabs of the target area, the others split it
enum class dock_zone : u8 { center, left, right, top, bottom };

// the side of a region an edge dock sits at
enum class dock_side : u8 { left, right, top, bottom };

struct tab_desc {
    std::string_view label;
    std::string_view icon; // optional; drawn with the icon font passed to tab_bar
    constexpr tab_desc(const char* l) noexcept : label{l} {}
    constexpr tab_desc(std::string_view l, std::string_view i = {}) noexcept : label{l}, icon{i} {}
};

class context;

// converts to false when the window is collapsed; end_window always runs
class window_scope {
public:
    window_scope(context& ctx, bool open) noexcept : ctx_{&ctx}, open_{open} {}
    ~window_scope();

    window_scope(const window_scope&)            = delete;
    window_scope& operator=(const window_scope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return open_; }

private:
    context* ctx_;
    bool     open_;
};

class font_scope {
public:
    explicit font_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~font_scope();

    font_scope(const font_scope&)            = delete;
    font_scope& operator=(const font_scope&) = delete;

private:
    context* ctx_;
};

class rich_scope {
public:
    explicit rich_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~rich_scope();

    rich_scope(const rich_scope&)            = delete;
    rich_scope& operator=(const rich_scope&) = delete;

private:
    context* ctx_;
};

class selectable_text_scope {
public:
    explicit selectable_text_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~selectable_text_scope();

    selectable_text_scope(const selectable_text_scope&)            = delete;
    selectable_text_scope& operator=(const selectable_text_scope&) = delete;

private:
    context* ctx_;
};

class modal_scope {
public:
    modal_scope(context& ctx, bool open) noexcept : ctx_{&ctx}, open_{open} {}
    ~modal_scope();

    modal_scope(const modal_scope&)            = delete;
    modal_scope& operator=(const modal_scope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return open_; }

private:
    context* ctx_;
    bool     open_;
};

// end_menu() (menu) / end_popup_menu() (popup menu, context menu) / end_main_menu_bar() when it goes out of scope
class menu_scope {
public:
    enum class kind : u8 { submenu, popup, main_bar };
    menu_scope(context& ctx, bool open, kind k) noexcept : ctx_{&ctx}, open_{open}, kind_{k} {}
    ~menu_scope();

    menu_scope(const menu_scope&)            = delete;
    menu_scope& operator=(const menu_scope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return open_; }

private:
    context* ctx_;
    bool     open_;
    kind     kind_;
};

class style_scope {
public:
    style_scope(context& ctx, u32 colors, u32 vars) noexcept : ctx_{&ctx}, colors_{colors}, vars_{vars} {}
    ~style_scope();

    style_scope(const style_scope&)            = delete;
    style_scope& operator=(const style_scope&) = delete;

private:
    context* ctx_;
    u32      colors_;
    u32      vars_;
};

class child_scope {
public:
    child_scope(context& ctx, bool open) noexcept : ctx_{&ctx}, open_{open} {}
    ~child_scope();

    child_scope(const child_scope&)            = delete;
    child_scope& operator=(const child_scope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return open_; }

private:
    context* ctx_;
    bool     open_;
};

class card_scope {
public:
    card_scope(context& ctx, bool open) noexcept : ctx_{&ctx}, open_{open} {}
    ~card_scope();

    card_scope(const card_scope&)            = delete;
    card_scope& operator=(const card_scope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return open_; }

private:
    context* ctx_;
    bool     open_;
};

class transition_scope {
public:
    explicit transition_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~transition_scope();

    transition_scope(const transition_scope&)            = delete;
    transition_scope& operator=(const transition_scope&) = delete;

private:
    context* ctx_;
};

class tree_scope {
public:
    tree_scope(context& ctx, bool open) noexcept : ctx_{&ctx}, open_{open} {}
    ~tree_scope();

    tree_scope(const tree_scope&)            = delete;
    tree_scope& operator=(const tree_scope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return open_; }

private:
    context* ctx_;
    bool     open_;
};

class context {
public:
    [[nodiscard]] static std::expected<context, font_error> create(const context_config& cfg = {});

    context(font_atlas atlas, const strata::style& theme, draw_list_limits limits = {});
    ~context();
    context(context&&) noexcept            = default;
    context& operator=(context&&) noexcept = default;

    // frame lifecycle ------------------------------------------------------
    void begin_frame(const input_state& input);
    // zeroes the typed-text bytes of the temporary once read: begin_frame(platform.new_frame())
    void begin_frame(input_state&& input);
    void end_frame();
    [[nodiscard]] draw_data render_data() const noexcept { return dl_.data(); }

    // true while the pointer is over a window / open popup or a widget is dragged: the host should not use mouse input
    [[nodiscard]] bool want_capture_mouse() const noexcept
    {
        return hovered_window_prev_ != 0 || active_ != 0 || dock_chrome_prev_ || toast_hover_prev_ || modal_count_ != 0 ||
               menu_hit_prev_ || (popup_open_prev_ && popup_rect_prev_.contains(mouse_));
    }
    // true while a text field has keyboard focus: the host should not treat keys as hotkeys
    [[nodiscard]] bool want_text_input() const noexcept { return focus_id_ != 0 || hotkey_capture_ != 0; }
    // a popup (combo list, color picker) is open: it handles Esc itself
    [[nodiscard]] bool popup_open() const noexcept { return popup_open_prev_; }
    // pointer shape the ui wants right now (resize grips, text fields); apply it with win32_platform::set_cursor
    [[nodiscard]] cursor_kind cursor() const noexcept { return cursor_; }
    // enter was pressed in a text field during this frame
    [[nodiscard]] bool input_submitted() const noexcept { return submitted_; }

    void set_clipboard(const clipboard_hooks& hooks) noexcept { clipboard_ = hooks; }

    // dpi / ui scale -------------------------------------------------------
    // the ui lays out in logical pixels; `scale` physical pixels make one. input arrives in physical pixels and is
    // converted, draw commands leave in physical pixels.
    [[nodiscard]] f32 scale() const noexcept { return scale_; }
    // scale 0.5 .. 4. rebuilds the font atlas (slow): the host then re-uploads it (renderer.update_atlas(ui.font())) and
    // calls release_font_pixels(); font_generation() changes so it can tell. on error nothing changes. font data given to
    // create() must still be alive; a context not made by create() cannot rescale.
    std::expected<void, font_error> set_scale(f32 scale);
    [[nodiscard]] u32 font_generation() const noexcept { return font_generation_; }

    // windows --------------------------------------------------------------
    [[nodiscard]] window_scope window(std::string_view title, vec2 initial_pos, f32 width = 280.0f)
    {
        return {*this, begin_window(title, initial_pos, width)};
    }
    bool begin_window(std::string_view title, vec2 initial_pos, f32 width = 280.0f);
    // size.y == 0: the height follows the content, otherwise the content scrolls. the size is remembered when resizable.
    [[nodiscard]] window_scope window(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags)
    {
        return {*this, begin_window(title, initial_pos, size, flags)};
    }
    bool begin_window(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags);
    void end_window();

    // docking --------------------------------------------------------------
    // windows with window_flags::dockable can be dragged by the title bar into a dock space (centre = join as a tab,
    // edges = split). up to 8 spaces exist at once; one you stop calling every frame is gone. without a space nothing docks.
    //   dock_area(rect), dock_area("name", rect)   the main space / a named one over any region
    //   dock_edge(name, side, size, region)        a resizable panel along a side of `region` that takes room only while
    //                                              windows are docked in it. returns what is left, so calls chain:
    //       rect client = ui.dock_edge("explorer", dock_side::left, 260, {{0, bar_h}, ui.display_size()});
    //       ui.dock_area(ui.dock_edge("console", dock_side::bottom, 200, client));
    //   floating_dock(title, pos, size)            a movable panel that is itself a dock space
    void dock_area(const rect& area);
    void dock_area(std::string_view space, const rect& area);
    [[nodiscard]] rect dock_edge(std::string_view space, dock_side side, f32 size, const rect& region);
    bool floating_dock(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags = window_flags::none);
    // docks `title` next to `target` (which must already be docked) or, with no target, into `space` (empty = the main
    // one), e.g. for a default layout. `size` is the share of the target's area an edge zone gives the new window.
    bool dock_window(std::string_view title, dock_zone zone, std::string_view target = {}, f32 size = 0.5f,
                     std::string_view space = {});
    void undock_window(std::string_view title);
    // the whole arrangement (spaces, splits, tabs, edge sizes, window positions) as text for dock_load_layout(), which
    // may be called before the windows have been shown. windows and spaces are matched by name: unknown ones are skipped,
    // ones missing from the text keep floating. returns false and changes nothing if the text is not a layout or has
    // more than 32 panes.
    [[nodiscard]] std::string dock_save_layout() const;
    bool dock_load_layout(std::string_view text);
    [[nodiscard]] bool is_docked(std::string_view title) const noexcept;
    // screen rectangle of a window as of its last begin_window / end_window ({} if it does not exist)
    [[nodiscard]] rect window_rect(std::string_view title) const noexcept;

    // widgets --------------------------------------------------------------
    void text(std::string_view s) { text_colored(style_.text, s); }
    void text_dim(std::string_view s) { text_colored(style_.text_dim, s); }
    void text_colored(color c, std::string_view s);
    // like text(), wrapped at word boundaries to the width of the layout (or set_next_item_width)
    void text_wrapped(std::string_view s) { text_wrapped_colored(style_.text, s); }
    void text_wrapped_colored(color c, std::string_view s);

    template <class... args>
    void textf(std::format_string<args...> fmt, args&&... a)
    {
        std::array<char, 512> buf;
        const auto r = std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()), fmt, std::forward<args>(a)...);
        text({buf.data(), std::min<std::size_t>(static_cast<std::size_t>(r.size), buf.size())});
    }

    bool button(std::string_view label);
    bool checkbox(std::string_view label, bool& value);
    bool toggle(std::string_view label, bool& value);
    void progress_bar(f32 fraction, std::string_view overlay = {});
    void separator();
    void spacing(f32 height = 0.0f);
    void same_line() noexcept { layout_.same_line = true; }
    // continue on the current line, `offset_x` pixels from the left edge of the content area
    void same_line(f32 offset_x) noexcept
    {
        layout_.same_line = true;
        layout_.cursor_x  = layout_.origin.x + offset_x - style_.item_spacing;
    }
    // width of the next widget (slider, input, combo, progress, button)
    void set_next_item_width(f32 w) noexcept { layout_.next_width = w; }

    // icon widgets: `icon` is drawn with `icon_font` (see icons.hpp), the label with the current font
    bool icon_button(font_id icon_font, std::string_view icon, std::string_view label = {});
    void icon_label(font_id icon_font, std::string_view icon, std::string_view label);

    // slider: label above on the left, value on the right, thin track with a round knob below
    template <class T>
        requires std::integral<T> || std::floating_point<T>
    bool slider(std::string_view label, T& value, T lo, T hi)
    {
        f32 tmp = static_cast<f32>(value);
        if constexpr (std::integral<T>) {
            const bool moved = slider_f32(label, tmp, static_cast<f32>(lo), static_cast<f32>(hi), 0);
            const T next = static_cast<T>(std::lround(tmp));
            const bool changed = moved && next != value;
            if (changed) { value = next; }
            return changed;
        } else {
            const bool changed = slider_f32(label, tmp, static_cast<f32>(lo), static_cast<f32>(hi), 2);
            if (changed) { value = static_cast<T>(tmp); }
            return changed;
        }
    }

    // single-line text field, true when the text changed. `hint` is shown while it is empty.
    bool input_text(std::string_view label, std::string& value, std::string_view hint = {},
                    input_flags flags = input_flags::none, std::size_t max_bytes = 4096);
    // for secrets: same, into a secure_string whose memory is zeroed whenever it is freed or reallocated
    bool input_text(std::string_view label, secure_string& value, std::string_view hint = {},
                    input_flags flags = input_flags::none, std::size_t max_bytes = 4096);
    // fixed buffer flavour: `buffer` is null-terminated, `capacity` includes the terminator
    bool input_text(std::string_view label, char* buffer, std::size_t capacity, std::string_view hint = {},
                    input_flags flags = input_flags::none);

    // multi-line text field: Enter starts a new line, Ctrl+Enter submits. size.x == 0 is the available width, size.y == 0
    // about six lines. true when the text changed.
    bool input_multiline(std::string_view label, std::string& value, vec2 size = {},
                         input_flags flags = input_flags::none, std::string_view hint = {},
                         std::size_t max_bytes = std::size_t{1} << 20);
    bool input_multiline(std::string_view label, secure_string& value, vec2 size = {},
                         input_flags flags = input_flags::none, std::string_view hint = {},
                         std::size_t max_bytes = std::size_t{1} << 20);

    // styles ranges of the NEXT input_text / input_multiline (syntax highlighting, live markup preview). recompute the
    // spans when the field reports a change; for the one frame in between the old ranges are clamped. passwords ignore
    // them, on overlap the first span wins, at most 4096 are used.
    void input_spans(std::span<const text_span> spans);

    // input methods: composed text is shown at the caret and only inserted once confirmed. while ime_wanted() the host
    // enables its IME and puts the candidate window at ime_position() (physical pixels, bottom left of the caret) with a
    // caret height of ime_line_height() (win32_platform::set_ime()).
    [[nodiscard]] bool             ime_wanted() const noexcept { return ime_want_; }
    [[nodiscard]] vec2             ime_position() const noexcept { return ime_pos_; }
    [[nodiscard]] f32              ime_line_height() const noexcept { return ime_line_h_; }
    [[nodiscard]] bool             ime_composing() const noexcept { return ime_len_ != 0; }
    [[nodiscard]] std::string_view ime_composition() const noexcept { return {ime_text_.data(), ime_len_}; }

    // dropdown. `current` is the selected index; returns true when it changed.
    bool combo(std::string_view label, int& current, const std::string_view* items, std::size_t count);
    bool combo(std::string_view label, int& current, std::initializer_list<std::string_view> items)
    {
        return combo(label, current, items.begin(), items.size());
    }

    // multi-select dropdown: `selected` points at `count` flags, one per item. the popup stays open while you click.
    // returns true when a flag changed.
    bool combo_multi(std::string_view label, bool* selected, const std::string_view* items, std::size_t count,
                     std::string_view placeholder = "none");
    bool combo_multi(std::string_view label, bool* selected, std::initializer_list<std::string_view> items,
                     std::string_view placeholder = "none")
    {
        return combo_multi(label, selected, items.begin(), items.size(), placeholder);
    }

    // row of tabs, true when `selected` changed. drawing the selected tab's content is up to the caller
    bool tab_bar(std::string_view id, const tab_desc* tabs, std::size_t count, int& selected, font_id icon_font = 0);
    bool tab_bar(std::string_view id, std::initializer_list<tab_desc> tabs, int& selected, font_id icon_font = 0)
    {
        return tab_bar(id, tabs.begin(), tabs.size(), selected, icon_font);
    }

    // color -------------------------------------------------------------
    // swatch + hex readout; clicking it opens a popup with the full picker
    bool color_edit(std::string_view label, color& c, color_flags flags = color_flags::none);
    // the picker itself, laid out inline: saturation / value square, hue bar, alpha bar, preview + hex field
    bool color_picker(std::string_view label, color& c, color_flags flags = color_flags::none);

    // trees and lists ------------------------------------------------------
    // if (ui.tree_node("Scene")) { ...children...; ui.tree_pop(); }   or   if (auto t = ui.tree("Scene")) {...}
    bool tree_node(std::string_view label, tree_flags flags = tree_flags::none);
    void tree_pop();
    [[nodiscard]] tree_scope tree(std::string_view label, tree_flags flags = tree_flags::none)
    {
        return {*this, tree_node(label, flags)};
    }
    bool tree_leaf(std::string_view label, bool selected = false);         // returns true when clicked
    bool selectable(std::string_view label, bool selected = false);        // full-width row, true when clicked
    [[nodiscard]] bool item_pressed() const noexcept { return item_pressed_; } // last tree row / selectable was clicked
    void text_ellipsis(std::string_view s); // like text(), but cut with "..." to the available width

    // tables ---------------------------------------------------------------
    //   if (ui.begin_table("files", 2)) {
    //       ui.table_setup_column("Name");                              // stretch column
    //       ui.table_setup_column("Size", 80);                          // fixed 80 px (a third argument sets the stretch weight)
    //       int sort = ui.table_headers_row(sort_column, ascending);    // the clicked column or -1
    //       for (auto& r : rows) {
    //           if (ui.table_next_row()) {                              // false when scrolled out of view: skip its cells
    //               ui.table_next_column(); ui.text(r.name);
    //               ui.table_next_column(); ui.textf("{}", r.size);
    //           }
    //       }
    //       ui.end_table();
    //   }
    // height > 0 makes the body a scrolling region under a fixed header.
    bool begin_table(std::string_view id, u32 columns, table_flags flags = table_default, f32 height = 0.0f);
    void table_setup_column(std::string_view label, f32 fixed_width = 0.0f, f32 stretch_weight = 1.0f);
    int  table_headers_row(int sort_column = -1, bool ascending = true);
    bool table_next_row();
    bool table_next_column();
    void end_table();

    // rich text ------------------------------------------------------------
    // text with inline markup: <f=N>..</f> switches to font id N, <c=rrggbb[aa]>..</c> colors. tags nest, "<<" is a
    // literal '<':   ui.rich_text("normal <f=2>heading</f> <c=ff8800>orange</c> 1 << 2");
    void rich_text(std::string_view markup);
    // the same, wrapped at word boundaries to the width of the layout (or set_next_item_width)
    void rich_text_wrapped(std::string_view markup);

    // while active, widget labels (buttons, checkboxes, tabs, tree rows, cards, tooltips, text() ...) are read as the same
    // markup. widget ids still come from the raw string:   auto rich = ui.rich_labels();  ui.button("<f=1>save</f>");
    void push_rich_labels() noexcept { ++rich_depth_; }
    void pop_rich_labels() noexcept  { if (rich_depth_ > 0) { --rich_depth_; } }
    [[nodiscard]] rich_scope rich_labels() noexcept { push_rich_labels(); return rich_scope{*this}; }
    [[nodiscard]] bool       rich_labels_active() const noexcept { return rich_depth_ > 0; }

    template <class... args>
    void rich_textf(std::format_string<args...> fmt, args&&... a)
    {
        std::array<char, 512> buf;
        const auto r = std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()), fmt, std::forward<args>(a)...);
        rich_text({buf.data(), std::min<std::size_t>(static_cast<std::size_t>(r.size), buf.size())});
    }

    // containers -----------------------------------------------------------
    // a clipped, scrollable area inside the current window. size.x == 0 takes the rest of the line's width, size.y == 0
    // fills down to the bottom of a fixed-height window. always pair with end_child(), or use the scoped child().
    bool begin_child(std::string_view id, vec2 size = {}, child_flags flags = child_flags::none);
    void end_child();
    [[nodiscard]] child_scope child(std::string_view id, vec2 size = {}, child_flags flags = child_flags::none)
    {
        return {*this, begin_child(id, size, flags)};
    }

    // group card: a titled box around related options that grows with its content
    bool begin_card(std::string_view title, std::string_view icon = {}, font_id icon_font = 0);
    void end_card();
    [[nodiscard]] card_scope card(std::string_view title, std::string_view icon = {}, font_id icon_font = 0)
    {
        return {*this, begin_card(title, icon, icon_font)};
    }

    // vertical tab strip (a sidebar), true when the selection changed. height 0 fills down to the bottom of a fixed-height
    // window; width 0 sizes to the labels (or to the icons with icons_only).
    bool tab_strip(std::string_view id, const tab_desc* tabs, std::size_t count, int& selected, font_id icon_font = 0,
                   f32 width = 0.0f, tab_strip_flags flags = tab_strip_flags::none, f32 height = 0.0f);
    bool tab_strip(std::string_view id, std::initializer_list<tab_desc> tabs, int& selected, font_id icon_font = 0,
                   f32 width = 0.0f, tab_strip_flags flags = tab_strip_flags::none, f32 height = 0.0f)
    {
        return tab_strip(id, tabs.begin(), tabs.size(), selected, icon_font, width, flags, height);
    }

    // images ---------------------------------------------------------------
    // a renderer texture drawn `size` big. size.x == 0 takes the layout width, size.y == 0 makes it square.
    // [uv0, uv1] is the part shown, `tint` multiplies it.
    void image(texture_id tex, vec2 size, vec2 uv0 = {0.0f, 0.0f}, vec2 uv1 = {1.0f, 1.0f},
               color tint = {255, 255, 255, 255}, f32 rounding = 0.0f);
    // clickable image with a hover / press highlight; returns true when clicked
    bool image_button(std::string_view label, texture_id tex, vec2 size, vec2 uv0 = {0.0f, 0.0f},
                      vec2 uv1 = {1.0f, 1.0f}, color tint = {255, 255, 255, 255});

    // transitions and tooltips -------------------------------------------------
    // multiply the alpha of everything drawn until pop_alpha()
    void push_alpha(f32 a) noexcept { dl_.push_alpha(a); }
    void pop_alpha() noexcept { dl_.pop_alpha(); }
    // fade + slide the content that follows whenever `page` changes; keep the returned scope alive while drawing the page
    [[nodiscard]] transition_scope page_transition(std::string_view key, int page, f32 slide = 14.0f);

    // shows `text` next to the pointer once the last widget has been hovered for a moment
    void tooltip(std::string_view text);
    [[nodiscard]] bool item_hovered() const noexcept { return last_item_hovered_; }

    // key binding: click, then press a key or side mouse button. Esc cancels, Backspace / Delete clears.
    // `key_code` is a virtual-key code, 0 = unbound.
    bool hotkey(std::string_view label, u32& key_code);

    void push_id(std::string_view s) noexcept;
    void pop_id() noexcept;

    // drag sliders and number inputs ---------------------------------------
    // a number you drag sideways (Shift = fine, Alt = coarse) or click to type. `speed` is the change per pixel.
    // lo < hi clamps to that range and turns the field into a ruler spanning it (`speed` is then ignored).
    bool drag_float(std::string_view label, f32& value, f32 speed = 0.05f, f32 lo = 0.0f, f32 hi = 0.0f, int decimals = 2,
                    std::string_view suffix = {});
    bool drag_int(std::string_view label, int& value, f32 speed = 0.2f, int lo = 0, int hi = 0, std::string_view suffix = {});
    // 2 to 4 components side by side under one caption
    bool drag_float_n(std::string_view label, f32* values, int count, f32 speed = 0.05f, f32 lo = 0.0f, f32 hi = 0.0f,
                      int decimals = 2);
    bool drag_float2(std::string_view label, f32* v, f32 speed = 0.05f, f32 lo = 0.0f, f32 hi = 0.0f, int decimals = 2)
    {
        return drag_float_n(label, v, 2, speed, lo, hi, decimals);
    }
    bool drag_float3(std::string_view label, f32* v, f32 speed = 0.05f, f32 lo = 0.0f, f32 hi = 0.0f, int decimals = 2)
    {
        return drag_float_n(label, v, 3, speed, lo, hi, decimals);
    }
    bool drag_float4(std::string_view label, f32* v, f32 speed = 0.05f, f32 lo = 0.0f, f32 hi = 0.0f, int decimals = 2)
    {
        return drag_float_n(label, v, 4, speed, lo, hi, decimals);
    }
    // a text field that holds a number; half-written input leaves the value alone until it parses. - / + buttons when step > 0
    bool input_float(std::string_view label, f32& value, f32 step = 0.0f, int decimals = 3);
    bool input_int(std::string_view label, int& value, int step = 1);

    // plots ------------------------------------------------------------------
    // `values` may be a ring buffer: `offset` is the index of the oldest sample. lo / hi = plot_auto fit the data.
    void plot_lines(std::string_view label, std::span<const f32> values, vec2 size = {0.0f, 80.0f}, std::string_view overlay = {},
                    f32 lo = plot_auto, f32 hi = plot_auto, u32 offset = 0);
    void plot_histogram(std::string_view label, std::span<const f32> values, vec2 size = {0.0f, 80.0f},
                        std::string_view overlay = {}, f32 lo = plot_auto, f32 hi = plot_auto, u32 offset = 0);
    // a tiny chart without axes or caption, for inline use
    void sparkline(std::span<const f32> values, vec2 size = {80.0f, 20.0f}, color c = {0, 0, 0, 0}, u32 offset = 0);
    // several series in one chart with a legend (all series are read as long as the shortest)
    void plot(std::string_view label, std::span<const plot_series> series, vec2 size = {0.0f, 110.0f},
              plot_kind kind = plot_kind::lines, f32 lo = plot_auto, f32 hi = plot_auto, u32 offset = 0);
    // a full chart with axes and optional zoom / pan. the x range follows the data unless zoomed, the y range follows
    // the visible samples unless fixed (plot_axis::lo / hi) or zoomed.
    void plot(std::string_view label, std::span<const plot_series> series, const plot_options& options);
    void plot_reset_view(std::string_view label);
    // whether the user has zoomed / panned the chart, and the x range it showed last frame ({0, 0} before the first)
    [[nodiscard]] bool plot_zoomed(std::string_view label) const;
    [[nodiscard]] vec2 plot_x_range(std::string_view label) const;

    // selectable text ----------------------------------------------------------
    // wrapped text that can be selected with the mouse and copied. the one-argument form keys the selection on the text
    // itself, so text that changes every frame needs an explicit `id`.
    void text_selectable(std::string_view text) { text_selectable(text, text); }
    void text_selectable(std::string_view id, std::string_view text);
    // while alive, text() / text_dim() / textf() / text_wrapped() draw selectable text
    void push_selectable_text() noexcept { ++selectable_depth_; }
    void pop_selectable_text() noexcept  { if (selectable_depth_ > 0) { --selectable_depth_; } }
    [[nodiscard]] selectable_text_scope selectable_text() noexcept { push_selectable_text(); return selectable_text_scope{*this}; }

    // modal windows and dialogs ---------------------------------------------------
    // open_modal("Confirm") once, then every frame: if (auto m = ui.modal("Confirm")) { ... ui.close_modal(); }
    // a modal is centered, dims and blocks everything below it; modals stack and the one opened last has the input.
    void open_modal(std::string_view title);
    void close_modal();                 // closes the topmost
    [[nodiscard]] bool modal_open() const noexcept { return modal_count_ != 0; }
    [[nodiscard]] modal_scope modal(std::string_view title, vec2 size = {420.0f, 0.0f}, modal_flags flags = modal_flags::esc_closes)
    {
        return {*this, begin_modal(title, size, flags)};
    }
    bool begin_modal(std::string_view title, vec2 size = {420.0f, 0.0f}, modal_flags flags = modal_flags::esc_closes);
    void end_modal();
    // message dialog with buttons: 0 while open (or never opened), 1..n for the button pressed, -1 when dismissed with
    // Esc / a click outside. it closes itself on 1..n / -1.
    //   switch (ui.dialog("Delete?", "This cannot be undone.", {"Delete", "Cancel"})) { case 1: ...; }
    int dialog(std::string_view title, std::string_view message, std::initializer_list<std::string_view> buttons,
               modal_flags flags = modal_flags::esc_closes | modal_flags::backdrop_closes);

    // menus ------------------------------------------------------------------------------------
    // a bar across the top of the display:
    //   if (auto bar = ui.main_menu_bar()) { if (auto m = ui.menu("File")) { if (ui.menu_item("Open", "Ctrl+O")) ...; } }
    bool begin_main_menu_bar();
    void end_main_menu_bar();
    [[nodiscard]] menu_scope main_menu_bar() { return {*this, begin_main_menu_bar(), menu_scope::kind::main_bar}; }
    [[nodiscard]] f32 main_menu_bar_height() const noexcept { return menu_bar_h_; }
    // a menu in the bar or a submenu inside another menu
    bool begin_menu(std::string_view label, bool enabled = true);
    void end_menu();
    [[nodiscard]] menu_scope menu(std::string_view label, bool enabled = true)
    {
        return {*this, begin_menu(label, enabled), menu_scope::kind::submenu};
    }
    // rows of a menu; true when clicked (which closes all menus unless options.keep_open). '&' marks a mnemonic
    // ("&Open"; "&&" is a literal '&'); in the menu bar, Alt + the mnemonic opens a menu.
    bool menu_item(std::string_view label, std::string_view shortcut = {}, bool selected = false, bool enabled = true);
    bool menu_item(std::string_view label, bool& checked, std::string_view shortcut = {}, bool enabled = true);
    bool menu_item(std::string_view label, const menu_item_options& options);
    bool menu_item(std::string_view label, bool& checked, const menu_item_options& options);
    // true on the frame the combination ("Ctrl+O", "Alt+F4", "F5", "Del" ...) is pressed; modifiers must match exactly.
    // while a text field has the keyboard only Ctrl / Alt combinations count, except the ones the field uses (Ctrl+A/C/V/X/Z/Y)
    [[nodiscard]] bool accelerator(std::string_view combo) const;
    void menu_separator();
    // a popup menu that opens where you right-click the last widget, or the given area:
    //   if (auto m = ui.context_menu("row")) { if (ui.menu_item("Delete")) ...; }
    bool begin_context_menu(std::string_view id);
    bool begin_context_menu(std::string_view id, const rect& area);
    // the same without the right click: open_popup_menu("id", pos) yourself, then begin_popup_menu("id") every frame
    void open_popup_menu(std::string_view id, vec2 pos);
    bool begin_popup_menu(std::string_view id);
    void end_popup_menu();
    [[nodiscard]] menu_scope context_menu(std::string_view id) { return {*this, begin_context_menu(id), menu_scope::kind::popup}; }
    [[nodiscard]] menu_scope context_menu(std::string_view id, const rect& area)
    {
        return {*this, begin_context_menu(id, area), menu_scope::kind::popup};
    }
    [[nodiscard]] menu_scope popup_menu(std::string_view id) { return {*this, begin_popup_menu(id), menu_scope::kind::popup}; }
    [[nodiscard]] bool menu_is_open() const noexcept { return menu_open_[0].key != 0; }

    // toasts -----------------------------------------------------------------------------------
    // short-lived notifications stacked in a corner of the display. hovering one pauses it, clicking dismisses it.
    toast_handle toast(std::string_view text, toast_kind kind = toast_kind::info, f32 seconds = 3.5f) { return toast({}, text, kind, seconds); }
    toast_handle toast(std::string_view title, std::string_view text, toast_kind kind = toast_kind::info, f32 seconds = 3.5f);
    // the general form, with buttons and / or progress. such a toast is closed by its x, not by a click on its body.
    //   toast_handle h = ui.toast({.title = "Deleted", .text = "3 files", .seconds = 8, .actions = acts});
    //   every frame:  if (ui.toast_action(h) == 0) undo();
    //   h = ui.toast({.title = "Downloading", .seconds = 0, .progress = 0});  ui.toast_progress(h, 0.4f, "40%");
    toast_handle toast(const toast_options& options);
    // which button of the toast was pressed since the last call: its index, once; -1 if none (also after the toast closed)
    [[nodiscard]] int toast_action(toast_handle h);
    // fraction >= 1 completes the toast (it closes 2 s later); toast_busy makes the bar endless again
    void toast_progress(toast_handle h, f32 fraction, std::string_view text = {});
    void toast_close(toast_handle h);
    [[nodiscard]] bool toast_alive(toast_handle h) const noexcept;
    void clear_toasts() noexcept { toasts_.clear(); }
    void set_toast_corner(screen_corner corner) noexcept { toast_corner_ = corner; }
    [[nodiscard]] std::size_t toast_count() const noexcept { return toasts_.size(); }

    // log view ---------------------------------------------------------------------------------
    // a toolbar (filter, level, follow, copy, clear) over the lines of `log`. only visible rows are drawn, so long buffers
    // are cheap. size.y == 0 fills down to the bottom of a fixed-height window.
    void log_view(std::string_view id, log_buffer& log, vec2 size = {0.0f, 0.0f}, log_view_flags flags = log_view_flags::none);

    // docking animation -----------------------------------------------------------------------
    // panes slide to their new place when a window docks, undocks or a pane closes (off by default)
    void set_dock_animation(bool on) noexcept { dock_animation_ = on; }

    // modifier keys (as of this frame)
    [[nodiscard]] bool ctrl_down() const noexcept  { return mod_ctrl_; }
    [[nodiscard]] bool shift_down() const noexcept { return mod_shift_; }
    [[nodiscard]] bool alt_down() const noexcept   { return mod_alt_; }

    // custom drawing -------------------------------------------------------
    // the frame's draw list, in screen space. drawing lands between the widgets submitted before and after the call;
    // outside any window it ends up behind all windows.
    [[nodiscard]] draw_list& draw() noexcept { return dl_; }

    // reserves a slot in the layout and returns its rectangle plus hover / held / pressed state; draw into it with draw()
    [[nodiscard]] item_result custom_item(std::string_view label, vec2 size);

    // smooth per-key value chasing `target` at the theme's animation speed (or `speed`, in 1/seconds). starts at `target`
    [[nodiscard]] f32 animate(std::string_view key, f32 target, f32 speed = 0.0f);

    [[nodiscard]] f32  content_width() const noexcept { return layout_.width; }
    [[nodiscard]] f32  frame_height() const noexcept { return font_.line_height(current_font()) + style_.frame_padding.y * 2.0f; }
    [[nodiscard]] vec2 mouse_pos() const noexcept { return mouse_; }
    [[nodiscard]] vec2 display_size() const noexcept { return display_; }

    // fonts ----------------------------------------------------------------
    // ids follow context_config::font, then extra_fonts. the current font applies to text and widget sizes until popped;
    // window titles always use font 0.
    void push_font(font_id f) noexcept;
    void pop_font() noexcept;
    [[nodiscard]] font_scope with_font(font_id f) noexcept { push_font(f); return font_scope{*this}; }
    [[nodiscard]] font_id    current_font() const noexcept { return font_stack_[font_depth_]; }

    // style overrides ------------------------------------------------------
    // temporary theme changes for every widget submitted until popped. unbalanced pushes are undone at the next begin_frame
    void push_color(style_color which, color c) noexcept;
    void pop_color(u32 count = 1) noexcept;
    void push_var(style_var which, f32 value) noexcept;
    void pop_var(u32 count = 1) noexcept;

    // push several at once; they are popped when the returned scope ends
    //   auto danger = ui.style_overrides({override_color(style_color::accent, red), override_var(style_var::rounding, 14)});
    [[nodiscard]] style_scope style_overrides(std::initializer_list<style_override> list) noexcept;

    // access ---------------------------------------------------------------
    [[nodiscard]] strata::style&       theme() noexcept { return style_; }
    [[nodiscard]] const strata::style& theme() const noexcept { return style_; }
    [[nodiscard]] const font_atlas&    font() const noexcept { return font_; }
    [[nodiscard]] u64                  frame_index() const noexcept { return frame_; }
    // seconds of ui time: the sum of the frame deltas passed to begin_frame
    [[nodiscard]] f64                  time() const noexcept { return time_; }

    // zeroes and frees the cpu copy of the font bitmap; call once the renderer exists
    void release_font_pixels() noexcept { font_.discard_pixels(); }

private:
    struct interaction {
        bool hovered{};
        bool held{};
        bool pressed{};
    };

    struct anim_slot {
        id   key{};
        u64  last_frame{};
        f32  hover{};
        f32  active{};
        f32  toggle{};
        f32  custom{};
        bool custom_init{};
    };

    struct window_state {
        id   key{};
        vec2 pos{};
        f32  width{};
        f32  height{};      // 0: follows the content
        f32  content_h{};
        f32  scroll{};
        bool collapsed{};
        bool resizable{};
        bool size_set{};
        bool overflow{};    // content taller than the body last frame (fixed-height windows)
        // docking
        u32  dock{};        // 1 + index of the dock node the window sits in, 0 = floating
        u32  dock_order{};  // position among the tabs of its node
        vec2 float_size{};  // size to go back to when un-docked
        bool docked_now{};  // drawn as a docked window this frame
        f32  title_h{};
        u64  last_frame{};
        id   dock_owner{};     // docked in a floating dock: that dock's window (the windows stack together)
        bool menubar{};        // the main menu bar (no padding, above the other windows)
        u32  modal_level{};    // 1 + index in the modal stack, 0 = not a modal
        u8   title_len{};
        std::array<char, 48> title{}; // visible part of the title, for dock tabs
    };

    struct layout_state {
        vec2 origin{};
        f32  width{};
        f32  cursor_x{};
        f32  line_top{};
        f32  line_h{};
        f32  bottom{};
        f32  next_width{};
        f32  bound_bottom{}; // bottom of the space a fill-height child / strip may use (0 = unbounded)
        bool same_line{};
        bool first{true};
    };

    struct saved_color {
        style_color which{};
        color       previous{};
    };
    struct saved_var {
        style_var which{};
        f32       previous{};
    };

    // a contiguous slice of draw commands and who emitted it: windows are restacked and popups lifted by reordering runs
    struct cmd_run {
        u32 first{};
        u32 count{};
        u32 owner{};
    };

    struct field_layout {
        rect control;
        rect label_row;
        bool has_label{};
    };

    static constexpr u32 max_tree_depth    = 16;
    static constexpr u32 max_table_columns = 16;
    static constexpr u32 max_tables        = 32;
    static constexpr u32 max_table_depth   = 4;

    struct tree_frame {
        f32 arrow_x{};
        f32 children_top{};
        f32 saved_origin_x{};
        f32 saved_width{};
    };
    struct tree_state {
        id   key{};
        bool open{};
    };

    struct table_state { // persists across frames
        id                                 key{};
        u64                                last_frame{};
        u32                                columns{};
        std::array<f32, max_table_columns> frac{}; // column widths as fractions of the table width
        f32                                row_hint{};
        f32                                scroll{};
        f32                                content_h{};
        bool                               inited{};
    };
    struct table_column {
        std::string_view label;
        f32              fixed{};
        f32              weight{1.0f};
    };
    struct table_frame { // the table being built this frame
        bool                                      active{};
        table_state*                              state{};
        table_flags                               flags{};
        u32                                       ncols{};
        u32                                       setup_count{};
        std::array<table_column, max_table_columns> cols{};
        std::array<f32, max_table_columns + 1>    col_x{};
        f32                                       height_limit{};
        vec2                                      origin{};
        f32                                       width{};
        layout_state                              outer{};
        f32                                       pad_x{}, pad_y{}, min_row_h{};
        f32                                       row_y{};
        f32                                       row_top{}, row_bottom{};
        u32                                       row_index{};
        int                                       col{-1};
        bool                                      columns_ready{};
        bool                                      header_done{};
        bool                                      body_started{};
        bool                                      in_row{};
        bool                                      row_visible{};
        bool                                      row_had_content{};
        bool                                      scroll_mode{};
        bool                                      clip_pushed{};
        bool                                      cell_clip{}; // the current cell has its own clip rectangle
        f32                                       body_top{}, body_h{};
    };

    struct child_state { // persists across frames
        id   key{};
        u64  last_frame{};
        f32  scroll{};
        f32  content_h{};
        bool overflow{};
    };
    struct child_frame { // the child being built
        child_state* state{};
        rect         bounds;
        rect         inner;
        layout_state outer{};
        child_flags  flags{};
    };
    struct card_state {
        id   key{};
        u64  last_frame{};
        f32  content_h{};
    };
    struct card_frame {
        card_state*  state{};
        layout_state outer{};
    };

    // dock tree: leaves hold windows as tabs, split nodes divide their area between two children
    static constexpr u32 max_dock_nodes = 32;
    static constexpr u8  no_node        = 0xff;
    struct dock_node {
        bool used{};
        u8   parent{no_node};
        std::array<u8, 2> child{no_node, no_node}; // both no_node: a leaf
        bool vertical{};   // false: children side by side, true: stacked
        f32  ratio{0.5f};  // share of the first child
        id   active{};     // leaf: the selected tab (window key)
        rect area;         // this frame: the whole node (the layout's target)
        rect content;      // this frame, leaf: below the tab bar
        rect shown_area;   // what is drawn: follows `area`, animated when dock animation is on
        rect shown_content;
        bool fresh{true};  // no shown_* yet
        u8   space{0};     // the dock space this node belongs to
        [[nodiscard]] bool leaf() const noexcept { return child[0] == no_node; }
    };
    struct dock_target {
        bool      valid{};
        u8        space{no_node};
        u8        node{no_node}; // no_node: the space is empty
        dock_zone zone{dock_zone::center};
        bool      outer{};       // splits the root instead of the node under the pointer
        rect      preview;
    };

    // a dock space: one tree of panes over a region. the main area, named areas, edge docks and floating docks are all spaces
    static constexpr u32 max_dock_spaces = 8;
    struct dock_space {
        id   key{};          // 0 = free slot
        u8   root{no_node};
        rect area;           // where its panes are laid out this frame
        rect drop;           // where a dragged window is accepted (a strip for an empty edge dock)
        rect panel;          // the whole panel an empty edge dock would take: the drop preview
        f32  edge_size{};    // edge docks: their width / height
        id   owner{};        // a floating dock: the key of its window
        bool set{};          // given a rectangle this frame
        bool hidden{};       // a collapsed floating dock: what is docked in it is hidden
        bool edge{};
        u64  last_frame{};
    };

    // text editing: undo history of the focused field
    enum class edit_kind : u8 { other, typing, erase_back, erase_fwd };
    struct edit_op {
        std::size_t pos{};      // where the change happened
        std::size_t off{};      // in the history text: removed bytes, then inserted bytes
        std::size_t rem_len{};
        std::size_t ins_len{};
        std::size_t cursor_before{};
        std::size_t anchor_before{};
        std::size_t cursor_after{};
        edit_kind   kind{};
        f64         time{};
    };
    struct edit_history {
        std::vector<edit_op> ops;
        secure_string        text; // the typed text lives here, so it is zeroed like the edit buffer
    };
    struct ml_line {
        u32 start{}; // byte range of the line, without its newline
        u32 end{};
    };

    // rich text layout scratch
    struct rich_run {
        std::string_view text;
        font_id          font{};
        color            col;
        text_flags       style{};
    };
    struct rich_seg {
        std::string_view text;
        font_id          font{};
        color            col;
        text_flags       style{};
        f32              x{};
    };
    struct rich_line {
        u32 first{};
        u32 count{};
        f32 width{};
        f32 asc{};
        f32 height{};
        f32 y{};
    };

    // menus: the chain of open popups (0 = the root: a context menu or a menu of the bar, 1.. = submenus)
    static constexpr u32 max_menu_levels = 4;
    struct menu_level {
        id   key{};
        vec2 pos{};       // where the popup wants to open
        rect anchor{};    // the header / row it opened from: pressing it is not "outside"
        vec2 size{};      // measured last frame
        rect rect_cur{};
        rect rect_prev{};
        f32  age{};
        bool seen{};      // built this frame
        bool from_bar{};
    };
    struct menu_frame { // a popup being built
        layout_state saved_layout{};
        u32          prev_owner{};
        f32          saved_spacing{};
        f32          content_w{};
        u32          level{};
        bool         saved_overlay{};
        bool         keys_ok{};   // the deepest open popup: letter keys activate mnemonics here
    };
    struct toast_entry {
        std::string title;
        std::string text;
        toast_kind  kind{};
        f32         duration{};
        f32         age{};
        f32         anim{};   // slide / fade in, 0..1
        bool        dismissed{};
        u64         seq{};
        std::vector<std::string> actions;
        f32         progress{-1.0f};
        bool        sticky{};       // no timer until it completes / is closed
        f32         busy_phase{};
    };
    std::vector<std::pair<u64, int>> toast_results_; // (toast, button) pressed and not yet read

    static constexpr u32 max_children = 32;
    static constexpr u32 max_child_depth = 4;
    static constexpr u32 max_cards = 48;
    static constexpr u32 max_card_depth = 4;

    static constexpr u32 max_windows   = 32;
    static constexpr u32 max_id_depth  = 16;
    static constexpr u32 max_overrides = 32;
    static constexpr u32 max_font_depth = 8;
    static constexpr u32 max_runs      = 256;
    static constexpr u32 no_z          = 0xffffffffu;
    static constexpr u32 run_base      = 0xffffffffu;
    static constexpr u32 run_overlay   = 0xfffffffeu;
    static constexpr u32 run_backdrop  = 0xfffffff0u; // + modal level - 1: the dimmed area behind a modal window

    [[nodiscard]] id       current_seed() const noexcept { return id_stack_[id_depth_]; }
    [[nodiscard]] anim_slot& anim_for(id key) noexcept;            // finds or creates (and keeps alive)
    [[nodiscard]] anim_slot* anim_find(id key) noexcept;           // finds only
    void                     anim_rehash() noexcept;
    struct press_anim { f32 hover{}; f32 active{}; };
    // idle buttons own no animation state
    [[nodiscard]] press_anim button_anim(id key, const interaction& in) noexcept;
    [[nodiscard]] f32      approach(f32 current, f32 target, f32 speed = 0.0f) const noexcept;
    [[nodiscard]] window_state* window_for(id key, vec2 pos, f32 width) noexcept;
    [[nodiscard]] rect     layout_place(vec2 size) noexcept;
    [[nodiscard]] field_layout layout_field(std::string_view shown_label, f32 control_height);
    [[nodiscard]] interaction interact(id key, const rect& r) noexcept;
    [[nodiscard]] color&   color_ref(style_color which) noexcept;
    [[nodiscard]] f32&     var_ref(style_var which) noexcept;
    [[nodiscard]] shape_style widget_shape(color base, f32 radius) const noexcept;
    [[nodiscard]] u32      z_index(id key) const noexcept;
    void                   bring_to_front(id key) noexcept;
    void                   switch_run(u32 owner) noexcept;
    void                   apply_layer_order();
    [[nodiscard]] bool     pointer_over(const rect& r) const noexcept;
    bool begin_popup_at(id key, const rect& anchor, vec2 size);
    void end_popup();
    bool picker_body(id key, color& c, color_flags flags);
    void draw_checker(const rect& r, f32 cell);
    bool row_item(id key, std::string_view shown, bool selected, f32 text_indent, f32 row_height);
    [[nodiscard]] bool& tree_open_state(id key, bool default_open);
    [[nodiscard]] table_state* table_for(id key) noexcept;
    void table_finalize_columns();
    void table_recompute_x() noexcept;
    void table_start_body();
    void table_finish_row();
    [[nodiscard]] f32 layout_next_y() const noexcept;
    void draw_tooltip(std::string_view text);
    void wipe_edit_buffer() noexcept;
    // every change of the focused field goes through edit_replace (which records undo history)
    bool edit_replace(std::size_t pos, std::size_t len, std::string_view with, edit_kind kind);
    bool edit_delete_selection();
    bool edit_insert(std::string_view s, bool typed);
    bool edit_undo();
    bool edit_redo();
    bool edit_shortcut(const key_event& ev, bool password, bool multiline); // ctrl+a / c / x / v / z / y
    void edit_history_clear() noexcept;
    void edit_history_add(edit_history& h, const edit_op& op, std::string_view removed, std::string_view inserted);
    bool input_multiline_core(std::string_view label, std::string_view current, vec2 size, input_flags flags,
                              std::string_view hint, std::size_t max_bytes);
    void ml_layout(std::string_view text, f32 width, font_id font, bool wrap);
    [[nodiscard]] std::size_t ml_line_of(std::size_t index) const noexcept;
    // rich_layout fills rich_lines_ / rich_segs_ and returns the size; rich_draw paints them
    vec2 rich_layout(std::string_view text, font_id base, color base_col, f32 wrap_width, bool markup);
    void rich_draw(vec2 pos);
    [[nodiscard]] vec2 label_size(font_id f, std::string_view s);
    void               label_draw(vec2 pos, color c, std::string_view s, font_id f);
    // docking
    void dock_compute(u8 node, const rect& area) noexcept;
    void dock_relayout() noexcept;
    [[nodiscard]] u8   dock_new_node() noexcept;
    void dock_free_node(u8 node) noexcept;
    [[nodiscard]] dock_target dock_pick(vec2 pointer, u8 exclude_leaf = no_node) const noexcept;
    void dock_attach(window_state& w, u8 space, u8 node, dock_zone zone, bool outer, f32 size = 0.0f) noexcept;
    [[nodiscard]] u8 dock_space_at(id key) noexcept;                 // finds or creates the space with this key
    [[nodiscard]] static id dock_space_key(std::string_view name) noexcept;
    void dock_set_space(u8 space, const rect& area, const rect& drop, const rect& panel) noexcept;
    void dock_detach(window_state& w) noexcept;
    void dock_prune() noexcept;
    void dock_end_frame();
    [[nodiscard]] window_state* window_find(id key) noexcept;
    [[nodiscard]] const window_state* window_find(id key) const noexcept;
    [[nodiscard]] interaction interact_impl(id key, const rect& r, bool in_window) noexcept;
    bool drag_box(std::string_view id_label, const rect& box, f32& value, f32 speed, f32 lo, f32 hi, int decimals,
                  std::string_view suffix, bool integer);
    void plot_impl(std::string_view label, std::span<const plot_series> series, vec2 size, plot_kind kind, f32 lo, f32 hi,
                   u32 offset, std::string_view overlay, bool compact);
    void draw_tooltip_at(vec2 anchor, std::string_view text);
    // the background of a popup (menu, dropdown, tooltip, toast): the given opaque shape, or frosted glass over what is
    // behind it when style_.popup_acrylic > 0
    void popup_panel(const rect& r, const shape_style& body);
    void draw_mnemonic(vec2 pos, color c, std::string_view text, std::size_t at, std::size_t len, bool underline, font_id f);
    bool menu_begin_level(u32 level, id key);
    void menu_end_level();
    void menu_open_root(id key, vec2 pos, const rect& anchor, bool from_bar);
    void menu_close_from(u32 level) noexcept;
    void menu_close_all() noexcept { menu_close_from(0); }
    [[nodiscard]] bool over_open_menu(vec2 p) const noexcept;
    void toast_end_frame();
    void dock_animate();
    bool slider_f32(std::string_view label, f32& value, f32 lo, f32 hi, int decimals);
    bool input_core(std::string_view label, std::string_view current, std::string_view hint, input_flags flags,
                    std::size_t max_bytes);
    void draw_combo_popup(id key, const rect& anchor, const std::string_view* items, std::size_t count,
                          int& current, bool& changed);

    font_atlas font_;
    draw_list  dl_;
    strata::style style_;

    // what set_scale needs to rebuild the atlas (strings are owned, the font_config views are re-pointed)
    struct font_source {
        std::string face;
        std::string file;
        font_config cfg;
    };
    std::vector<font_source> font_sources_;
    u32  max_atlas_size_{4096};
    f32  scale_{1.0f};
    u32  font_generation_{1};

    u64  frame_{};
    f64  time_{};
    f32  dt_{1.0f / 60.0f};
    vec2 display_{};
    vec2 mouse_{};
    vec2 mouse_delta_{};
    f32  wheel_{};
    bool mouse_down_{};
    bool mouse_pressed_{};
    bool mouse_released_{};
    bool have_mouse_{};

    std::array<key_event, max_key_events> keys_{};
    u32                                   key_count_{};
    std::array<char, max_typed_bytes>     typed_{};
    u32                                   typed_len_{};
    clipboard_hooks                       clipboard_{};

    id active_{};
    cursor_kind cursor_{};
    bool        wheel_consumed_{}; // an inner scroller (table, popup list) used this frame's wheel
    id hovered_window_prev_{};
    id hovered_window_cur_{};
    u32 hovered_z_{no_z};
    id  focused_window_{}; // topmost window of the previous frame

    window_state* cur_{};
    id            cur_window_{};
    layout_state  layout_{};

    std::array<id, max_id_depth + 1> id_stack_{};
    u32                              id_depth_{};

    std::array<saved_color, max_overrides> color_stack_{};
    std::array<saved_var, max_overrides>   var_stack_{};
    u32                                    color_depth_{};
    u32                                    var_depth_{};

    std::array<font_id, max_font_depth + 1> font_stack_{};
    u32                                     font_depth_{};

    // back-to-front window stacking, persistent across frames
    std::array<id, max_windows> z_order_{};
    u32                         z_count_{};
    // windows submitted this frame, in call order
    std::array<window_state*, max_windows> frame_windows_{};
    u32                                    frame_window_count_{};

    // draw-command runs of this frame, in emission order
    std::array<cmd_run, max_runs> runs_{};
    u32                           run_count_{};
    u32                           run_owner_{run_base};
    u32                           run_start_{};
    bool                          runs_overflow_{};

    // input extras
    bool          mouse_right_down_{};
    bool          mouse_right_pressed_{};
    bool          mod_ctrl_{};
    bool          mod_shift_{};
    bool          mod_alt_{};

    // number widgets
    id            number_edit_{};       // the drag field being typed into
    std::string   number_buf_;
    f32           drag_frac_{};         // integer drags: the part of a step not applied yet
    vec2          drag_start_{};        // where the drag field was pressed
    bool          drag_moved_{};        // the pointer left the dead zone around it: this is a drag, not a click
    bool          input_rect_set_{};    // the next input_core takes this rectangle instead of laying itself out
    rect          input_rect_{};

    // selectable text
    u32           selectable_depth_{};
    color         ml_color_{0, 0, 0, 0}; // text color for the next input_multiline_core (alpha 0: the theme's)
    std::vector<vec2> plot_scratch_;
    // the view a zoomable chart is showing (zoom / pan survive between frames)
    struct chart_view {
        id   key{};
        bool x_set{};
        bool y_set{};
        f32  x_lo{}, x_hi{}, y_lo{}, y_hi{}; // the user's view
        f32  sx_lo{}, sx_hi{}, sy_lo{}, sy_hi{}; // what was drawn last frame
        rect inner;                          // ... and where
        f64  last_click{-10.0};
        u64  last_frame{};
    };
    std::array<chart_view, 16> chart_views_{};
    [[nodiscard]] chart_view& chart_view_for(id key) noexcept;
    void chart_impl(std::string_view label, std::span<const plot_series> series, const plot_options& o);

    // modals
    static constexpr u32 max_modals = 4;
    std::array<id, max_modals> modal_stack_{};
    u32           modal_count_{};
    u32           next_window_modal_level_{};
    bool          next_window_menubar_{};
    struct modal_frame {
        bool alpha_pushed{};
    };
    std::array<modal_frame, max_modals> modal_frames_{};
    u32           modal_depth_{};       // modals being built right now
    id            modal_top_prev_{};    // the modal that has the input (as of the last frame)

    // menus
    std::array<menu_level, max_menu_levels> menu_open_{};
    std::array<menu_frame, max_menu_levels> menu_stack_{};
    u32           menu_depth_{};
    bool          menu_close_all_{};
    bool          menu_hit_prev_{};     // the pointer is over an open menu (previous frame's rectangles)
    bool          in_menu_bar_{};
    f32           menu_bar_h_{};
    f32           menu_bar_saved_spacing_{};

    // toasts
    std::vector<toast_entry> toasts_;
    screen_corner toast_corner_{screen_corner::bottom_right};
    bool          toast_hover_cur_{};
    bool          toast_hover_prev_{};
    u64           toast_seq_{};

    // docking animation
    bool          dock_animation_{};
    bool          dock_splitting_{}; // a splitter is being dragged: panes follow the pointer without easing

    // text input
    edit_history  undo_;
    edit_history  redo_;
    bool          edit_history_on_{};
    bool          edit_readonly_{};
    std::size_t   edit_max_bytes_{};
    u64           edit_version_{};     // bumped on every change of the edit buffer
    f32           edit_pref_x_{-1.0f}; // multi-line: column kept while moving up / down
    std::vector<ml_line> ml_lines_;
    u64           ml_cache_key_{};
    id          focus_id_{};
    bool        focus_seen_{};
    bool        press_claimed_{};
    bool        submitted_{};
    secure_string edit_buf_; // the live text of the focused field; zeroed as soon as focus goes away
    std::size_t edit_cursor_{};
    std::size_t edit_anchor_{};
    f32         edit_scroll_{};
    f64         caret_time_{};
    f64         last_click_time_{-10.0};
    vec2        last_click_pos_{};
    u32         click_count_{};      // 1, 2, 3 for single / double / triple clicks (a fourth starts over)
    [[nodiscard]] u32 register_click() noexcept;

    // styled contents (input_spans), for the field being built
    std::vector<text_span> edit_spans_pending_;
    std::vector<text_span> edit_spans_;
    u64         edit_spans_hash_{};
    void ed_prepare_spans(std::size_t text_size, font_id base, f32& line_h, f32& ascent, bool ignore);
    [[nodiscard]] f32 ed_measure(font_id base, std::string_view t, std::size_t a, std::size_t b) const;
    [[nodiscard]] font_id ed_font_at(font_id base, std::size_t i) const noexcept;
    void ed_draw(vec2 pos, f32 line_ascent, color col, std::string_view t, std::size_t a, std::size_t b, font_id base);

    // input method: the composition (from input_state) and what the focused field tells the host
    std::array<char, 256> ime_text_{};
    u32         ime_len_{};
    u32         ime_cursor_{};
    bool        ime_want_{};
    vec2        ime_pos_{};
    f32         ime_line_h_{};
    void draw_ime_chip(vec2 caret_bottom, f32 line_h, font_id f);

    // popup (combo)
    id   popup_id_{};
    bool in_overlay_{};
    bool popup_open_cur_{};
    bool popup_open_prev_{};
    rect popup_rect_cur_{};
    rect popup_rect_prev_{};
    rect popup_anchor_cur_{};  // the combo box that owns the popup
    rect popup_anchor_prev_{};
    bool swallow_press_{};     // this frame's press only closed a popup: widgets must ignore it
    f32  popup_scroll_{};
    int  popup_hover_{-1};

    // color picker (the one being edited)
    id    pick_key_{};
    f32   pick_h_{}, pick_s_{}, pick_v_{};
    color pick_last_{};
    // generic popup content
    layout_state popup_saved_layout_{};
    u32          popup_prev_owner_{run_base};

    // trees, tables
    std::array<tree_frame, max_tree_depth> tree_stack_{};
    u32                                    tree_depth_{};
    std::vector<tree_state>                tree_states_; // sorted by key
    bool                                   item_pressed_{};
    std::array<table_state, max_tables>    tables_{};
    table_frame                            table_{};
    std::array<table_frame, max_table_depth> table_stack_{}; // the tables around the current one
    u32                                    table_depth_{};

    // rich text
    u32                                    rich_depth_{};
    std::vector<rich_run>                  rich_runs_;
    std::vector<rich_seg>                  rich_segs_;
    std::vector<rich_line>                 rich_lines_;

    // docking
    std::array<dock_node, max_dock_nodes>  dock_nodes_{};
    std::array<dock_space, max_dock_spaces> dock_spaces_{};
    bool                                   dock_any_set_{};   // some space got a rectangle this frame
    u32                                    dock_counter_{};
    dock_target                            dock_target_cur_{};
    dock_target                            dock_target_prev_{};
    id                                     dock_drag_win_{};  // the floating dockable window being dragged this frame
    id                                     dock_drag_prev_{}; // ... and in the previous one (a drop happens on the release frame)
    bool                                   dock_chrome_cur_{};
    bool                                   dock_chrome_prev_{};
    bool                                   hovered_docked_cur_{};
    bool                                   hovered_docked_prev_{};
    vec2                                   dock_press_pos_{};
    u8                                     dock_group_src_{no_node}; // a whole pane (its tabs) is being dragged by the grip of its tab bar
    bool                                   dock_group_moved_{};

    // child regions, cards
    std::array<child_state, max_children>  children_{};
    std::array<child_frame, max_child_depth> child_stack_{};
    u32                                    child_depth_{};
    std::array<card_state, max_cards>      cards_{};
    std::array<card_frame, max_card_depth> card_stack_{};
    u32                                    card_depth_{};

    // window flags of the window being built, and its frame (for drag_by_body)
    window_flags cur_flags_{};
    rect         cur_frame_{};

    // tooltips: what the pointer rests on
    id   last_item_key_{};
    bool last_item_hovered_{};
    id   hover_key_cur_{};
    id   hover_key_prev_{};
    f32  hover_time_{};

    // hotkey binding
    u32  pressed_key_{};
    id   hotkey_capture_{};
    bool hotkey_seen_{};

    std::array<window_state, max_windows> windows_{};
    std::vector<anim_slot>                anims_;     // open addressing, power-of-two size, grows on demand
    u32                                   anim_used_{}; // occupied slots, live or stale
};

} // namespace strata
