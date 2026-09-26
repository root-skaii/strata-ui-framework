#pragma once

#include "strata/datetime.hpp"
#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/log.hpp"
#include "strata/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstring>
#include <expected>
#include <format>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
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
    // horizontal wheel: a tilt wheel, a trackpad sideways gesture (WM_MOUSEHWHEEL). positive = towards the right,
    // which is the opposite sign convention to `wheel` because that is what the message reports.
    f32                 wheel_x{};
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
    // every key held right now, one bit per windows virtual-key code, so an app can ask about keys the ui itself
    // does not use (Delete, F2, ...) without polling the os. a host that cannot fill this leaves it zeroed and
    // context::key_down() then always says no; context::key_pressed(vk) works either way, from `pressed_key`.
    std::array<u8, 32>                    keys_held{};

    [[nodiscard]] constexpr bool held(u32 virtual_key) const noexcept
    {
        return virtual_key < 256 && (keys_held[virtual_key >> 3] & (1u << (virtual_key & 7))) != 0;
    }
    constexpr void set_held(u32 virtual_key, bool down) noexcept
    {
        if (virtual_key >= 256) { return; }
        const u8 bit = static_cast<u8>(1u << (virtual_key & 7));
        if (down) { keys_held[virtual_key >> 3] |= bit; }
        else      { keys_held[virtual_key >> 3] = static_cast<u8>(keys_held[virtual_key >> 3] & ~bit); }
    }
};

// readable name of a windows virtual-key code ("A", "F5", "Space", "Mouse 4" ...); "None" for 0
[[nodiscard]] std::string_view key_name(u32 virtual_key) noexcept;

// a key (virtual-key code or extra mouse button, see input_state::pressed_key) with the exact modifiers held with it
struct key_chord {
    u32  key{};    // 0 = unbound
    bool ctrl{};
    bool shift{};
    bool alt{};

    [[nodiscard]] constexpr bool bound() const noexcept { return key != 0; }
    friend constexpr bool operator==(const key_chord&, const key_chord&) noexcept = default;
};

// "Ctrl+Shift+S": modifiers first, then key_name(). empty for an unbound chord
[[nodiscard]] std::string chord_to_string(const key_chord& chord);
// the inverse, also for the accelerator() syntax ("ctrl + s", "Alt+F4"). "" gives an unbound chord; false (and `out`
// untouched) if a part is not known
[[nodiscard]] bool chord_from_string(std::string_view text, key_chord& out) noexcept;

// seconds allowed between the steps of a key_sequence, both pressing one (sequence_pressed) and capturing one (hotkey_sequence)
inline constexpr f64 key_sequence_timeout = 1.5;

// a chord, or the first few of a short sequence of chords pressed one after another ("Ctrl+K" then "Ctrl+S"), each
// within key_sequence_timeout of the one before. a plain key_chord converts to a one-step sequence, so code and
// config that only ever used single chords keeps working
struct key_sequence {
    static constexpr u32 max_steps = 3;

    std::array<key_chord, max_steps> steps{};
    u8                                count{};

    constexpr key_sequence() noexcept = default;
    constexpr key_sequence(const key_chord& c) noexcept : steps{c}, count{c.bound() ? u8{1} : u8{0}} {}

    [[nodiscard]] constexpr bool bound() const noexcept { return count > 0 && steps[0].bound(); }
    friend constexpr bool operator==(const key_sequence&, const key_sequence&) noexcept = default;
};

// "Ctrl+K, Ctrl+S": chord_to_string() of each step, joined with ", ". empty for an unbound sequence
[[nodiscard]] std::string sequence_to_string(const key_sequence& seq);
// the inverse: comma-separated chords, each in the chord_from_string() syntax. "" gives an unbound sequence; false
// (and `out` untouched) if a step does not parse or there are more than key_sequence::max_steps of them
[[nodiscard]] bool sequence_from_string(std::string_view text, key_sequence& out) noexcept;

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
    f32  tooltip_delay_s = 0.4f; // how long the pointer has to rest on a widget before its tooltip shows
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
    acrylic_saturation, acrylic_brightness, popup_acrylic, tooltip_delay,
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

// which mouse button an item_clicked() question is about
enum class mouse_button : u8 { left, right, middle };

// what a frame cost (context::stats(), filled at end_frame and describing the frame that just ended)
struct frame_stats {
    u32 items_submitted{};  // rows / widgets that went through the cullable item path
    u32 items_culled{};     // ... of those, the ones outside the clip rectangle, which did no work
    u32 vertices{};
    u32 indices{};
    u32 draw_calls{};
    u32 text_measures{};    // label_size() calls that had to walk the string
    u32 measure_hits{};     // ... and the ones the measurement cache answered
    u32 anim_slots_used{};  // occupied slots of the animation table (live and stale)
    u32 anim_slots_total{}; // its capacity
    // things that went wrong quietly. all of them mean the ui drew something other than what was asked for, and all
    // of them used to be invisible: geometry dropped because a reservation ran out (raise draw_list_limits), a
    // nesting deeper than the clip / alpha stacks hold, and widget ids that collided (see id_collisions).
    u32 draw_overflow{};       // 1 if the vertex / index / command / shape arrays ran out
    u32 clip_overflows{};
    u32 alpha_overflows{};
    u32 id_collisions{};    // ids submitted more than once this frame (debug builds only; see id_collision())
    // whether anything the renderer sees changed since the previous frame, and whether animations are still moving.
    // a host that owns its window can skip the present when both say no; see context::frame_unchanged().
    bool unchanged{};
    bool animating{};
    f64 begin_frame_ms{};
    f64 end_frame_ms{};
};

enum class input_flags : u16 {
    none                = 0,
    password            = 1, // shows bullets, disables copy / cut / undo
    read_only           = 2, // selectable and copyable, not editable
    select_all_on_focus = 4,
    no_wrap             = 8, // input_multiline: do not wrap long lines, scroll sideways instead
    no_frame            = 16, // input_multiline: no background, border or padding (the text sits directly in the layout)
    auto_height         = 32, // input_multiline: as tall as its text, so it never scrolls
    reveal              = 64, // with password: an eye button at the right end shows the text while it is toggled on
    clear_button        = 128, // a small x at the right end while the field has text; pressing it empties the field
};

[[nodiscard]] constexpr input_flags operator|(input_flags a, input_flags b) noexcept
{
    return static_cast<input_flags>(static_cast<u16>(a) | static_cast<u16>(b));
}

// what input_code adds to a multi-line field
enum class code_flags : u8 {
    none           = 0,
    line_numbers   = 1,  // a gutter with the line numbers
    highlight_line = 2,  // a faint bar behind the line the caret is on
    bracket_match  = 4,  // the bracket next to the caret and the one it pairs with are boxed
    auto_indent    = 8,  // Enter keeps the indentation (and adds a level after an opening bracket), a typed } steps back
    find_replace   = 16, // Ctrl+F / Ctrl+H open a find / replace bar above the text
};

[[nodiscard]] constexpr code_flags operator|(code_flags a, code_flags b) noexcept
{
    return static_cast<code_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

inline constexpr code_flags code_default =
    code_flags::line_numbers | code_flags::highlight_line | code_flags::bracket_match | code_flags::auto_indent | code_flags::find_replace;

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

// what the pointer should look like, for hosts that can change it (win32_platform::set_cursor).
// `hand` marks something that will act on a click (a rich-text link); `not_allowed` a disabled item under the
// pointer, which is the difference between "this does nothing" and "this is broken"; `resize_nesw` is the other
// diagonal, so a window's bottom-left grip does not show the bottom-right arrow.
enum class cursor_kind : u8 { arrow, text, hand, not_allowed, resize_ew, resize_ns, resize_nwse, resize_nesw };

enum class child_flags : u8 {
    none         = 0,
    frame        = 1, // draw a rounded background and border
    no_padding   = 2,
    no_scrollbar = 4, // still clips and scrolls with the wheel, without the scrollbar
    acrylic      = 8, // frosted glass background (see window_flags::acrylic)
    // content wider than the region scrolls sideways instead of being cut off: a horizontal scrollbar along the
    // bottom, the tilt wheel (input_state::wheel_x) and Shift + wheel. full-width widgets still size themselves to
    // the visible width, so what overflows is what asked to be wide -- a table with fixed columns, an image, a long
    // unwrapped line. see scroll_x() / set_scroll_x().
    horizontal   = 16,
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
    hideable    = 16, // right-click the header for a menu that shows / hides columns
    reorderable = 32, // drag a header sideways to move its column
};

[[nodiscard]] constexpr table_flags operator|(table_flags a, table_flags b) noexcept
{
    return static_cast<table_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

inline constexpr table_flags table_default =
    table_flags::striped | table_flags::borders | table_flags::resizable | table_flags::row_hover;

enum class table_column_flags : u8 {
    none           = 0,
    default_hidden = 1, // hidden until the user shows it (table_flags::hideable)
    no_hide        = 2, // the menu cannot hide it
    no_reorder     = 4, // it stays where it is: it cannot be dragged, and columns do not move past it
};

[[nodiscard]] constexpr table_column_flags operator|(table_column_flags a, table_column_flags b) noexcept
{
    return static_cast<table_column_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

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

// context::confirm: what the dialog shows beyond its message
struct confirm_options {
    std::string_view title;            // empty: "Confirm"
    // a "don't ask again" checkbox under the message. while *remember is true the dialog does not open at all and
    // confirm() answers `remembered` straight away, which is the "stop asking me" setting every tool grows
    bool*            remember{nullptr};
    std::string_view remember_label{"Don't ask again"};
    int              remembered{1};    // the button confirm() reports while *remember is true (1 = the first)
    int              danger{0};        // 1..n: that button is drawn in the warning colour
};

enum class toast_kind : u8 { info, success, warning, error };

// the color a kind is drawn in (toasts, badges): green / amber / red, and the theme's accent for info
[[nodiscard]] color kind_color(toast_kind kind, const style& theme) noexcept;

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

// a small pill: a label with an optional close button and / or selected state (see context::chip)
struct chip_options {
    bool             closable{false};   // a small x at the right; pressing it reports `closed`, removing the chip is up to you
    bool*            selected{nullptr}; // toggled by a click on the body: a chip that acts as a filter / tag toggle
    color            tint{0, 0, 0, 0};  // alpha 0: the theme's accent
    std::string_view icon;              // a glyph drawn with `icon_font` before the label
    font_id          icon_font{0};
};

struct chip_result {
    bool clicked{};
    bool closed{};
};

// what drop_target() found: a payload of the wanted type is over the widget (`hovering`), and it was let go there (`dropped`)
struct drop_result {
    bool                 hovering{};
    bool                 dropped{};
    vec2                 local{};  // the pointer inside the widget, 0..1 on both axes (top / bottom half for an insert position)
    std::span<const u8>  data{};   // the payload bytes, valid when dropped

    [[nodiscard]] explicit operator bool() const noexcept { return dropped; }
    template <class T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] T as() const noexcept
    {
        T value{};
        if (data.size() == sizeof(T)) { std::memcpy(&value, data.data(), sizeof(T)); }
        return value;
    }
};

enum class drop_flags : u8 {
    none         = 0,
    no_highlight = 1, // do not outline the widget while a matching payload is over it (draw your own insert marker)
};

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
    // what tells this tab apart from the others, when the caption is not stable: a tab whose text gains a dirty dot,
    // a pin marker or a count keeps its place, its selection and its drag state only if it keeps its `id`. empty =
    // the label is the identity, which is how it always worked.
    std::string_view id;
    constexpr tab_desc(const char* l) noexcept : label{l} {}
    constexpr tab_desc(std::string_view l, std::string_view i = {}) noexcept : label{l}, icon{i} {}
    constexpr tab_desc(std::string_view l, std::string_view i, std::string_view key) noexcept : label{l}, icon{i}, id{key} {}
    [[nodiscard]] constexpr std::string_view key() const noexcept { return id.empty() ? label : id; }
};

enum class tab_bar_flags : u8 {
    none        = 0,
    closable    = 1, // an x on every tab
    reorderable = 2, // drag a tab sideways to move it
    add_button  = 4, // a "+" after the last tab
};

[[nodiscard]] constexpr tab_bar_flags operator|(tab_bar_flags a, tab_bar_flags b) noexcept
{
    return static_cast<tab_bar_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

// what happened in a tab bar this frame. the bar only reports: removing and moving tabs in your own list is up to you
// (apply_tab_events does both, and keeps `selected` on the tab it was on). tabs are told apart by their label, so labels
// have to be unique
struct tab_events {
    bool changed{};      // `selected` changed
    int  closed{-1};     // the tab whose x was pressed
    int  moved_from{-1}; // the tab that was dragged past a neighbour, and the index it belongs at now
    int  moved_to{-1};
    bool add{};          // the "+" was pressed
};

// applies tab_events to a list of tabs and the selected index
template <class T>
void apply_tab_events(const tab_events& ev, std::vector<T>& items, int& selected)
{
    const int size = static_cast<int>(items.size());
    if (ev.moved_from >= 0 && ev.moved_from < size && ev.moved_to >= 0 && ev.moved_to < size && ev.moved_from != ev.moved_to) {
        const int from = ev.moved_from;
        const int to   = ev.moved_to;
        T moved = std::move(items[static_cast<std::size_t>(from)]);
        items.erase(items.begin() + from);
        items.insert(items.begin() + to, std::move(moved));
        if (selected == from)                         { selected = to; }
        else if (from < selected && selected <= to)   { --selected; }
        else if (to <= selected && selected < from)   { ++selected; }
    }
    if (ev.closed >= 0 && ev.closed < static_cast<int>(items.size())) {
        items.erase(items.begin() + ev.closed);
        if (ev.closed < selected) { --selected; }
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(items.size()) - 1));
    }
}

// the selection of a list, with the modifier rules every list wants: a plain click selects one, Ctrl toggles,
// Shift takes the range from the last plain / Ctrl click. Indices are kept sorted; the app owns the object and
// hands it to context::selection_click() with the row that was clicked.
//   if (ui.selectable(rows[i].name, id, sel.contains(i))) { ui.selection_click(sel, i); }
class selection_state {
public:
    [[nodiscard]] bool contains(int index) const noexcept
    {
        return std::binary_search(items_.begin(), items_.end(), index);
    }
    [[nodiscard]] std::span<const int> items() const noexcept { return items_; }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    // the row a Shift range would grow from, -1 when there is none
    [[nodiscard]] int anchor() const noexcept { return anchor_; }

    void clear() noexcept { items_.clear(); anchor_ = -1; }
    void set_anchor(int index) noexcept { anchor_ = index; }
    void select_one(int index)
    {
        items_.assign(1, index);
        anchor_ = index;
    }
    void add(int index)
    {
        const auto at = std::lower_bound(items_.begin(), items_.end(), index);
        if (at == items_.end() || *at != index) { items_.insert(at, index); }
    }
    void remove(int index)
    {
        const auto at = std::lower_bound(items_.begin(), items_.end(), index);
        if (at != items_.end() && *at == index) { items_.erase(at); }
    }
    void toggle(int index)
    {
        if (contains(index)) { remove(index); } else { add(index); }
        anchor_ = index;
    }
    // [lo, hi] added to what is already there; the anchor stays where it was so dragging the range keeps working
    void add_range(int lo, int hi)
    {
        if (lo > hi) { std::swap(lo, hi); }
        for (int i = lo; i <= hi; ++i) { add(i); }
    }
    // drops everything outside [0, count): call it when the list it indexes into shrinks
    void clamp_to(int count)
    {
        std::erase_if(items_, [count](int i) { return i < 0 || i >= count; });
        if (anchor_ >= count) { anchor_ = items_.empty() ? -1 : items_.back(); }
    }

private:
    std::vector<int> items_;
    int              anchor_{-1};
};

class context;

namespace internal { // (src/dock_state.hpp)
struct dock_state;
struct dock_target;
struct dock_guide;
struct dock_space;
} // namespace internal

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

// nav_end() when it goes out of scope (see context::nav_begin)
class nav_scope {
public:
    explicit nav_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~nav_scope();

    nav_scope(const nav_scope&)            = delete;
    nav_scope& operator=(const nav_scope&) = delete;

private:
    context* ctx_;
};

// end_disabled() when it goes out of scope (see context::begin_disabled)
class disabled_scope {
public:
    explicit disabled_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~disabled_scope();

    disabled_scope(const disabled_scope&)            = delete;
    disabled_scope& operator=(const disabled_scope&) = delete;

private:
    context* ctx_;
};

// pop_right_gutter() when it goes out of scope (see context::push_right_gutter)
class gutter_scope {
public:
    explicit gutter_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~gutter_scope();

    gutter_scope(const gutter_scope&)            = delete;
    gutter_scope& operator=(const gutter_scope&) = delete;

private:
    context* ctx_;
};

// end_popup() when it goes out of scope; converts to false while the popup is closed
class popup_scope {
public:
    popup_scope(context& ctx, bool open) noexcept : ctx_{&ctx}, open_{open} {}
    ~popup_scope();

    popup_scope(const popup_scope&)            = delete;
    popup_scope& operator=(const popup_scope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return open_; }

private:
    context* ctx_;
    bool     open_;
};

// end_drag_source() when it goes out of scope; converts to true while the widget is being dragged
class drag_source_scope {
public:
    drag_source_scope(context& ctx, bool active) noexcept : ctx_{&ctx}, active_{active} {}
    ~drag_source_scope();

    drag_source_scope(const drag_source_scope&)            = delete;
    drag_source_scope& operator=(const drag_source_scope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return active_; }

private:
    context* ctx_;
    bool     active_;
};

class context {
public:
    [[nodiscard]] static std::expected<context, font_error> create(const context_config& cfg = {});

    context(font_atlas atlas, const strata::style& theme, draw_list_limits limits = {});
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;

    // frame lifecycle ------------------------------------------------------
    void begin_frame(const input_state& input);
    // zeroes the typed-text bytes of the temporary once read: begin_frame(platform.new_frame())
    void begin_frame(input_state&& input);
    void end_frame();
    [[nodiscard]] draw_data render_data() const noexcept
    {
        draw_data d = dl_.data();
        d.content_hash = geometry_hash_; // lets the renderers skip an upload of geometry they already hold
        return d;
    }

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
    // the same thing as a percentage, which is what a "UI scale" setting shows. 100 is one logical pixel per
    // physical pixel; the monitor's dpi scale is a sensible starting point, not a ceiling -- an overlay on a 4K
    // screen often wants 150 % of it. Steps of 5 or 10 are what a +/- pair should use; the atlas is rebuilt on every
    // change (tens of milliseconds per font), so drive it from a stepper or apply a slider when it is let go.
    [[nodiscard]] int scale_percent() const noexcept { return static_cast<int>(scale_ * 100.0f + 0.5f); }
    std::expected<void, font_error> set_scale_percent(int percent) { return set_scale(static_cast<f32>(percent) / 100.0f); }

    // windows --------------------------------------------------------------
    [[nodiscard]] window_scope window(std::string_view title, vec2 initial_pos, f32 width = 280.0f)
    {
        return {*this, begin_window(title, initial_pos, width)};
    }
    bool begin_window(std::string_view title, vec2 initial_pos, f32 width = 280.0f);
    // size.y == 0: the height follows the content, up to the bottom of the display (then it scrolls); otherwise the
    // window has that height and the content scrolls. the size is remembered when resizable.
    [[nodiscard]] window_scope window(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags)
    {
        return {*this, begin_window(title, initial_pos, size, flags)};
    }
    bool begin_window(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags);
    void end_window();

    // docking --------------------------------------------------------------
    // windows with window_flags::dockable can be dragged by the title bar into a dock space (centre = join as a tab,
    // edges = split). up to 8 spaces exist at once; one you stop calling every frame is gone. without a space nothing docks.
    // while dragging: drop guides show over a pane, the tab bar of a pane takes the window as a tab at the pointer, Shift
    // moves it without docking, Esc cancels docking. tabs reorder by dragging along their bar; double-click floats a tab
    // or resets a splitter / edge dock.
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
    // the same with the identity kept apart from the caption (see the selectable / tree_node pair below)
    bool button(std::string_view label, std::string_view id);
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
    // continue on the current line with an item `width` wide that ends at the right edge of the content area. the edge
    // already accounts for the window padding and for a scrollbar that is showing, so a right-aligned control does not
    // have to be placed by hand and does not move when either changes. inside a right gutter (push_right_gutter) the
    // edge is the real one, not the reduced one the items before it were laid out in.
    void same_line_right(f32 width) noexcept
    {
        layout_.same_line = true;
        layout_.cursor_x  = layout_.origin.x + layout_.width + layout_.gutter - width - style_.item_spacing;
    }
    // reserves `w` pixels at the right of the content area: everything laid out until pop_right_gutter() is that much
    // narrower (so a full-width row ends where the gutter starts and its label is ellipsized there), while
    // same_line_right() still reaches the real edge, which is what the gutter is for.
    //   { auto g = ui.right_gutter(60.0f); ui.custom_item("header", {0, 24}); }   // 60 px free for the row's buttons
    //   ui.same_line_right(24.0f); ui.icon_button(icons_font, icons::trash);
    void push_right_gutter(f32 w) noexcept;
    void pop_right_gutter() noexcept;
    [[nodiscard]] gutter_scope right_gutter(f32 w) noexcept { push_right_gutter(w); return gutter_scope{*this}; }
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

    // input mask: the field only takes what the mask allows and adds the fixed characters itself, so `value` always holds the
    // formatted text. in the mask  # a digit, A a letter, U / L a letter turned to upper / lower case, X a letter or digit,
    // ? anything, \ makes the next character a literal, everything else is a literal:
    //   ui.input_masked("phone", phone, "(###) ###-####");     ui.input_masked("plate", plate, "UU-###");
    // typing, deleting and pasting all keep the shape; the undo history is off for masked fields. returns true when the
    // text changed.
    bool input_masked(std::string_view label, std::string& value, std::string_view mask, std::string_view hint = {},
                      input_flags flags = input_flags::none);

    // a code editor: a multi-line field without wrapping, and (code_flags) line numbers, current line, bracket matching,
    // auto indent, Tab / Shift+Tab that indent and unindent the lines of a selection, and a find / replace bar (Ctrl+F,
    // Ctrl+H) that marks every match. colours come from input_spans() as usual. size.y == 0 is about 12 lines.
    bool input_code(std::string_view label, std::string& value, vec2 size = {}, code_flags flags = code_default,
                    std::string_view hint = {}, std::size_t max_bytes = std::size_t{1} << 22, int tab_size = 4);
    // scrolls a code field to a line (1 = the first) and, when it has the keyboard, puts the caret there; call in the id
    // scope of input_code
    void code_goto_line(std::string_view label, int line);
    // opens the find bar of a code field (with the replace row) and gives it the keyboard; `text` is what to search for
    // (empty: what was searched last). also in the id scope of input_code
    void code_find(std::string_view label, std::string_view text = {}, bool with_replace = false);

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

    // a dropdown with a filter box: the popup opens with the keyboard in a search field, the list narrows as you
    // type (case-insensitive substring) and only the rows in view are submitted, so a few thousand entries are fine.
    // Up / Down move through what is left, Enter picks, Esc closes. Returns true when `current` changed.
    bool combo_filtered(std::string_view label, int& current, const std::string_view* items, std::size_t count,
                        std::string_view hint = "type to filter");
    bool combo_filtered(std::string_view label, int& current, std::initializer_list<std::string_view> items,
                        std::string_view hint = "type to filter")
    {
        return combo_filtered(label, current, items.begin(), items.size(), hint);
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
    // the same with closable / reorderable tabs and an add button. any number of tabs: when they do not fit they scroll
    // (mouse wheel) and a list button at the right end opens a menu of all of them.
    //   auto ev = ui.tab_bar("docs", tabs.data(), tabs.size(), current, tab_bar_flags::closable | tab_bar_flags::reorderable);
    //   apply_tab_events(ev, tab_list, current);
    tab_events tab_bar(std::string_view id, const tab_desc* tabs, std::size_t count, int& selected, tab_bar_flags flags,
                       font_id icon_font = 0);

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
    void text_ellipsis(std::string_view s); // like text(), but cut with "..." to the available width (item_truncated())

    // small controls that belong to the row just submitted, laid into its right end: strata places them (right to
    // left, inside the scrollbar inset), clips them to the row, takes the press away from it and reports it. Tell the
    // row how much room to leave with set_next_item_gutter() before submitting it, and its own label is elided there
    // instead of running underneath.
    //   ui.set_next_item_gutter(56.0f);
    //   const bool row = ui.selectable(name, id, selected);
    //   if (ui.row_accessory_button(icon_font, icons::trash)) { destroy(); }
    //   ui.row_accessory_checkbox("on", object.enabled);
    bool row_accessory_button(font_id icon_font, std::string_view icon, std::string_view id = {});
    bool row_accessory_checkbox(std::string_view id, bool& value);
    bool row_accessory_toggle(std::string_view id, bool& value);
    // room reserved at the right end of the next row for its accessories (see above); one row only
    void set_next_item_gutter(f32 width) noexcept { next_gutter_ = width; }

    // applies a click on row `index` to a selection: plain = only this row, Ctrl = toggle it, Shift = the range from
    // the row the last plain / Ctrl click was on. Returns true when the selection changed.
    bool selection_click(selection_state& sel, int index) const;

    // the same rows with the identity kept apart from the text, for lists whose labels repeat: `id` is hashed after the
    // label and never shown, so a row can be told apart by its path or its object's address without building a
    // "name##suffix" string every frame. push_id(const void*) / push_id(u64) do the same for a whole subtree.
    //   ui.selectable(node.name, {reinterpret_cast<const char*>(&node), sizeof(void*)}, node.id == selected);
    bool tree_node(std::string_view label, std::string_view id, tree_flags flags);
    bool tree_leaf(std::string_view label, std::string_view id, bool selected);
    bool selectable(std::string_view label, std::string_view id, bool selected);
    [[nodiscard]] tree_scope tree(std::string_view label, std::string_view id, tree_flags flags)
    {
        return {*this, tree_node(label, id, flags)};
    }

    // the state of the next tree_node / table_tree_node, whatever it was before (a "expand to the selection" button)
    void set_next_item_open(bool open) noexcept { next_open_ = open ? 1 : 2; }
    // ... and of it and everything under it. Ctrl or Shift held while its arrow is clicked does the same.
    void set_next_item_open_recursive(bool open) noexcept { next_open_ = open ? 3 : 4; }
    // opens / closes every tree node whose id starts in the current id scope (push_id("scene") scopes it to that
    // subtree; at the top level it is every node of the ui). takes effect the next time they are submitted.
    void open_all_tree_nodes() noexcept { tree_set_bulk(current_seed(), true); }
    void close_all_tree_nodes() noexcept { tree_set_bulk(current_seed(), false); }

    // scrolling ------------------------------------------------------------
    // the innermost scrolling region being built (a child region, otherwise the window). offsets are logical pixels
    // from the top of the content; both are 0 for a region that does not scroll
    [[nodiscard]] f32 scroll_y() const noexcept;
    [[nodiscard]] f32 scroll_max_y() const noexcept;
    void set_scroll_y(f32 y) noexcept;
    // the same for the sideways offset of the innermost child region opened with child_flags::horizontal.
    // 0 everywhere else: windows and tables lay out to the width they are given and never overflow sideways.
    [[nodiscard]] f32 scroll_x() const noexcept;
    [[nodiscard]] f32 scroll_max_x() const noexcept;
    void set_scroll_x(f32 x) noexcept;
    void scroll_to_top() noexcept { set_scroll_y(0.0f); }
    void scroll_to_bottom() noexcept { set_scroll_y(scroll_max_y()); }
    // scrolls the least it has to for the last submitted item to be fully in view (nothing if it already is). the new
    // offset shows next frame, so calling it every frame while a row stays selected is what "reveal the selection" does
    void ensure_item_visible() noexcept;
    // ... and this one puts it in the middle of the view instead
    void scroll_to_item() noexcept;
    // reserves `height` of layout without submitting anything, for a caller that culls a run of rows itself (the
    // scrollbar and everything below stay where they would be). skip_items is the same for a block of equal rows
    void skip_item(f32 height);
    void skip_items(int count, f32 item_height);

    // keyboard navigation ---------------------------------------------------
    // opt-in for a list or tree: between nav_begin() and nav_end() the rows (selectable, tree_node, tree_leaf) form a
    // navigable sequence. Up / Down move the cursor, Home / End jump to the ends, PageUp / PageDown move by ten,
    // Left closes a node or steps out to its parent, Right opens it or steps in, and Enter activates the row the cursor
    // is on -- which reports it exactly like a click (selectable() returns true, item_pressed() is set). Clicking a row
    // moves the cursor to it. The cursor is kept per scope across frames and the view follows it.
    //   if (auto nav = ui.navigation("tree")) { ... rows ... }        // nav_end() when the scope ends
    // the scope only takes the keys while no text field has the keyboard, which is what nav_active() reports.
    void nav_begin(std::string_view id);
    void nav_end();
    [[nodiscard]] nav_scope navigation(std::string_view id) { nav_begin(id); return nav_scope{*this}; }
    // the navigable scope had the keyboard this frame (a text field takes precedence)
    [[nodiscard]] bool nav_active() const noexcept { return nav_.active; }
    // the last submitted item has the keyboard: it is where a nav scope's cursor sits, or it is the text field that
    // is being typed into
    [[nodiscard]] bool item_focused() const noexcept { return last_item_focused_ || (focus_id_ != 0 && focus_id_ == last_item_key_); }

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
    // table_flags::hideable / reorderable let the user hide and move columns (right-click / drag the header). the code
    // keeps filling the cells in the order it declared the columns; table_next_column() is false for a hidden one, and
    // table_headers_row returns the column that was clicked by its declared index.
    bool begin_table(std::string_view id, u32 columns, table_flags flags = table_default, f32 height = 0.0f);
    void table_setup_column(std::string_view label, f32 fixed_width = 0.0f, f32 stretch_weight = 1.0f,
                            table_column_flags flags = table_column_flags::none);
    int  table_headers_row(int sort_column = -1, bool ascending = true);
    bool table_next_row();
    bool table_next_column();
    void end_table();
    // skips `count` rows without submitting them (they are `row height` tall, so a scrollbar stays right): for big tables
    // together with list_clipper
    void table_skip_rows(int count);
    // the column order, widths and hidden columns of a table as text (for a config file) and back. call with the id
    // scope of begin_table; loading may happen before the table is first shown
    [[nodiscard]] std::string table_save_layout(std::string_view id) const;
    void table_load_layout(std::string_view id, std::string_view text);

    // tree tables: rows of a table with expandable nodes in one of the cells. call these inside a cell instead of text();
    // the children are the rows that follow (each with an indented leaf or node in the same column):
    //   ui.table_next_row(); ui.table_next_column();
    //   const bool open = ui.table_tree_node("src");    ui.table_next_column(); ui.text("folder");
    //   if (open) {
    //       ui.table_next_row(); ui.table_next_column(); (void)ui.table_tree_leaf("main.cpp"); ui.table_next_column(); ui.text("4 KB");
    //       ui.table_tree_pop();
    //   }
    bool table_tree_node(std::string_view label, tree_flags flags = tree_flags::none);
    bool table_tree_leaf(std::string_view label, bool selected = false);
    void table_tree_pop();

    // long lists -----------------------------------------------------------
    // only the rows that can be seen are submitted: see list_clipper. the two calls reserve the space of the rows above
    // and below them (rows of one height, `item_height` 0 = one line of text; in a table the row height of its rows)
    void list_clip_begin(int count, f32 item_height, int& first, int& last);
    void list_clip_end(int count, f32 item_height, int last);

    // rich text ------------------------------------------------------------
    // text with inline markup: <f=N>..</f> switches to font id N, <c=rrggbb[aa]>..</c> colors. tags nest, "<<" is a
    // literal '<':   ui.rich_text("normal <f=2>heading</f> <c=ff8800>orange</c> 1 << 2");
    // links: `<a=href>text</a>` inside rich_text / rich_text_wrapped draws `text` underlined in the accent colour
    // (a `<c=>` inside the link keeps its own colour), shows the hand cursor over it, and reports the href for the
    // frame it was clicked in. `href` is whatever you put there -- a url, a file, a command name; strata does not
    // open anything itself, since what a link should do is the application's business:
    //
    //   ui.rich_text("see <a=https://example.com>the manual</a> or <a=cmd:reset>reset</a>");
    //   if (auto href = ui.rich_link_clicked(); !href.empty()) { open(href); }
    //
    // links in widget captions (under rich_labels()) are drawn but not clickable: the widget owns the click.
    [[nodiscard]] std::string_view rich_link_clicked() const noexcept { return rich_clicked_; }
    // the href under the pointer right now, for a status bar or a tooltip
    [[nodiscard]] std::string_view rich_link_hovered() const noexcept { return rich_hovered_; }

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

    // shows `text` next to the pointer once the last widget has been hovered for style::tooltip_delay_s
    void tooltip(std::string_view text);
    [[nodiscard]] bool item_hovered() const noexcept { return last_item_hovered_; }

    // the last hit-tested widget (button, row, field, custom_item): where it is, and what was done to it. the right
    // and middle buttons are reported on the press (there is no press-and-hold state for them), the left one on the
    // release over the widget, exactly like the widget's own return value
    [[nodiscard]] rect item_rect() const noexcept { return last_item_rect_; }
    [[nodiscard]] bool item_clicked(mouse_button b = mouse_button::left) const noexcept;
    // a second click on it within the double-click time, e.g. "open" on a row that a single click selects
    [[nodiscard]] bool item_double_clicked() const noexcept { return last_item_double_; }

    // lets the widget that was just submitted share its rectangle with the ones after it: a later overlapping widget
    // takes the press away from it (so a button drawn on top of a full-width row is the one that gets clicked), and
    // its own hover highlight goes away while the pointer is over that later widget. without this the widget
    // submitted first keeps the press, which is how everything else in strata behaves.
    //   auto row = ui.custom_item("component", {0, 26});
    //   ui.allow_item_overlap();
    //   ui.same_line_right(24.0f);
    //   if (ui.icon_button(icons_font, icons::trash)) { remove(); }
    //   if (row.pressed && !ui.item_claimed()) { select(); }
    void allow_item_overlap() noexcept;
    // a later widget took the press from the one that called allow_item_overlap()
    [[nodiscard]] bool item_claimed() const noexcept { return overlap_stolen_; }

    // key binding: click, then press a key or side mouse button. Esc cancels, Backspace / Delete clears.
    // `key_code` is a virtual-key code, 0 = unbound.
    bool hotkey(std::string_view label, u32& key_code);
    // the same for a key with modifiers: hold Ctrl / Shift / Alt while pressing the key (a bare Esc cancels, a bare
    // Backspace / Delete unbinds)
    bool hotkey_chord(std::string_view label, key_chord& chord);
    // the same for a short sequence of chords ("Ctrl+K, Ctrl+S"): the first key held with modifiers commits right away
    // (as fast as hotkey_chord for the common one-step case), but the field keeps listening for key_sequence_timeout
    // longer, and a further key extends the sequence, again committing right away, up to key_sequence::max_steps. a
    // bare Esc as the very first key leaves it as it was, a bare Backspace / Delete as the very first key unbinds it
    bool hotkey_sequence(std::string_view label, key_sequence& seq);

    // everything submitted until pop_id() hashes its id under this one, so the same labels in different rows stay
    // apart. the non-string forms are for rows that stand for an object: the pointer or the key identifies the row,
    // and no "label##suffix" string has to be built for it.
    void push_id(std::string_view s) noexcept;
    void push_id(const void* p) noexcept;
    void push_id(u64 value) noexcept;
    void push_id(int value) noexcept { push_id(static_cast<u64>(static_cast<i64>(value))); }
    void pop_id() noexcept;

    // popups ---------------------------------------------------------------
    // a panel with ordinary widgets in it that opens under the last widget (or at `pos`) and closes on Esc or a click
    // outside it. one popup is open at a time (menus, dropdowns and this share the slot), so no popup inside a popup.
    // call open_popup / popup with the same id scope. `width` 0 is the width of the widget it opened from (at least 180).
    //   if (ui.button("options")) { ui.toggle_popup("opts"); }
    //   if (auto p = ui.popup("opts")) { ui.checkbox("wrap", wrap); if (ui.button("done")) { ui.close_popup(); } }
    void open_popup(std::string_view id);
    void open_popup(std::string_view id, vec2 pos);
    void toggle_popup(std::string_view id);
    bool begin_popup(std::string_view id, f32 width = 0.0f);
    void end_popup();
    void close_popup() noexcept { popup_id_ = 0; }
    [[nodiscard]] bool popup_is_open(std::string_view id) const noexcept;
    [[nodiscard]] popup_scope popup(std::string_view id, f32 width = 0.0f) { return {*this, begin_popup(id, width)}; }
    // the screen rectangle of the last widget that was hit-tested (button, field, row ...)
    [[nodiscard]] rect last_item_rect() const noexcept { return last_item_rect_; }

    // drag and drop ----------------------------------------------------------
    // pick something up with the mouse and drop it on a widget that wants it. the payload is a small copy of your data
    // (an index, an id) tagged with a type name; a target only takes the type it asks for. Esc cancels.
    //   ui.selectable(row.name);
    //   if (auto d = ui.drag_source("row", i)) { ui.text(row.name); }         // the preview that follows the pointer
    //   ...
    //   ui.selectable(other.name);
    //   if (auto drop = ui.drop_target("row")) { move_row(drop.as<int>(), j); }
    // begin_drag_source() / end_drag_source() are the unscoped form. the source widget does not report a click for
    // the release that ends its drag.
    [[nodiscard]] bool begin_drag_source();
    void set_drag_payload(std::string_view type, const void* data, std::size_t size);
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void set_drag_payload(std::string_view type, const T& value) { set_drag_payload(type, &value, sizeof(T)); }
    void end_drag_source();
    template <class T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] drag_source_scope drag_source(std::string_view type, const T& payload)
    {
        const bool active = begin_drag_source();
        if (active) { set_drag_payload(type, payload); }
        return {*this, active};
    }
    [[nodiscard]] drop_result drop_target(std::string_view type, drop_flags flags = drop_flags::none);
    [[nodiscard]] bool dragging() const noexcept { return dd_active_; }
    [[nodiscard]] std::string_view drag_payload_type() const noexcept { return dd_active_ ? std::string_view{dd_type_} : std::string_view{}; }

    // date and time pickers ---------------------------------------------------
    // a field showing the value; clicking it opens a popup: a month calendar (arrows for months and years, today marked,
    // a Today button) or grids of hours / minutes (and seconds) with - / + for the minutes in between. each returns true
    // when the value changed. see datetime.hpp for date, time_of_day, parsing and formatting
    bool date_picker(std::string_view label, date& value);
    bool time_picker(std::string_view label, time_of_day& value, bool seconds = false);
    bool datetime_picker(std::string_view label, date& d, time_of_day& t, bool seconds = false);

    // small status widgets ----------------------------------------------------
    // a rotating arc for work of unknown length; `diameter` 0 fits the line. put a label next to it with same_line()
    void spinner(f32 diameter = 0.0f, color c = {0, 0, 0, 0});
    // a non-interactive pill with a short text ("3", "new", "beta"); the kind picks the color
    void badge(std::string_view text, toast_kind kind = toast_kind::info);
    void badge(std::string_view text, color tint);
    // a removable / toggleable tag: a pill you can click, with an optional x. chips flow with same_line()
    chip_result chip(std::string_view label, const chip_options& options = {});

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

    // a confirmation that opens and closes itself, and remembers what it was asked about, so a caller needs neither a
    // "which object" member nor a "is it open" one:
    //   if (ui.button("Delete")) { ui.ask_confirm("del", "Delete this object?", object_id); }
    //   switch (ui.confirm("del", {"Delete", "Cancel"}, {.remember = &never_ask})) {
    //       case 1: destroy(static_cast<u32>(ui.confirm_data())); break;
    //   }
    // confirm() returns 0 while nothing is being asked, 1..n for the button that was pressed, -1 for Esc / a click
    // outside. With `remember` pointing at a flag that is true it never opens and answers `remembered` on the frame
    // ask_confirm() was called, so the "stop asking me" setting needs no special case in the caller.
    // `id` is global, like a modal's title: ask_confirm() from inside a window and confirm() from outside one mean
    // the same dialog
    void ask_confirm(std::string_view id, std::string_view message, u64 user_data = 0);
    int  confirm(std::string_view id, std::initializer_list<std::string_view> buttons, const confirm_options& options = {});
    // what ask_confirm() was given, valid while that confirm() is open and on the frame it answers
    [[nodiscard]] u64 confirm_data() const noexcept { return confirm_data_; }

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
    // the same for a chord you keep (see keybinds). never true while a hotkey field is waiting for a key
    [[nodiscard]] bool chord_pressed(const key_chord& chord) const;
    // true on the frame the last step of the sequence is pressed, each step within sequence_timeout of the one before
    // (Esc, or pausing longer than that, drops what was pressed so far). a one-step sequence behaves like chord_pressed
    [[nodiscard]] bool sequence_pressed(const key_sequence& seq) const;
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
    // panes slide to their new place when a window docks, undocks or a pane closes (on by default)
    void set_dock_animation(bool on) noexcept { dock_animation_ = on; }

    // true when the key was pressed this frame with exactly these modifiers (auto-repeat included). a text field that has
    // the keyboard uses its keys up while it is drawn, so ask before it (or use accelerator() / chord_pressed())
    [[nodiscard]] bool key_pressed(key k, bool ctrl = false, bool shift = false) const noexcept
    {
        for (u32 i = 0; i < key_count_; ++i) {
            if (keys_[i].k == k && keys_[i].ctrl == ctrl && keys_[i].shift == shift) { return true; }
        }
        return false;
    }
    // the text field with this label (in the current id scope) takes the keyboard the next time it is drawn, its
    // text selected. Calling it every frame is harmless: a field that already has the keyboard is left alone, so the
    // caret is not taken back on every keystroke and no "did I ask already" flag is needed.
    void request_text_focus(std::string_view label) noexcept
    {
        const id key = hash_id(label, current_seed());
        if (focus_id_ != key) { focus_request_ = key; }
    }
    // which text field has the keyboard: the field's own id (0 = none), and the question by label
    [[nodiscard]] id   focused_field() const noexcept { return focus_id_; }
    [[nodiscard]] bool field_focused(std::string_view label) const noexcept
    {
        return focus_id_ != 0 && focus_id_ == hash_id(label, current_seed());
    }

    // raw keys, for shortcuts the ui itself has no name for (Delete, F2, F5 ...). `virtual_key` is a windows
    // virtual-key code, so 'A'..'Z', '0'..'9' and VK_* all work. key_pressed is the edge (this frame only) and needs
    // nothing from the host beyond input_state::pressed_key; key_down is the level and needs input_state::keys_held,
    // which win32_platform fills. Both stay quiet while a text field or a hotkey field has the keyboard, so a bare
    // Delete does not destroy the selection while a name is being typed.
    [[nodiscard]] bool key_pressed(u32 virtual_key, bool ctrl = false, bool shift = false, bool alt = false) const noexcept;
    [[nodiscard]] bool key_down(u32 virtual_key) const noexcept;

    // mouse buttons: 0 left, 1 right, 2 middle. `clicked` / `released` are the edges of this frame. These are the raw
    // buttons, not tied to any widget -- ask item_clicked() when you mean "on the thing I just submitted".
    [[nodiscard]] bool mouse_down(int button) const noexcept;
    [[nodiscard]] bool mouse_clicked(int button) const noexcept;
    [[nodiscard]] bool mouse_released(int button) const noexcept;

    // which window has the keyboard. window_focused() is about the window being submitted right now, so a panel can
    // scope its shortcuts to itself:  if (ui.window_focused() && ui.key_pressed(VK_DELETE)) { destroy(); }
    [[nodiscard]] bool window_focused() const noexcept;
    [[nodiscard]] bool is_window_focused(std::string_view title) const noexcept;

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
    // ... with the identity kept apart from the label (see the selectable / tree_node pair)
    [[nodiscard]] item_result custom_item(std::string_view label, std::string_view id, vec2 size);
    // draws `s` at `pos` cut with "..." once it would pass `max_width` -- what text_ellipsis() does, for a custom item
    // that paints its own row. returns the width actually drawn.
    f32 label_clipped(vec2 pos, f32 max_width, color c, std::string_view s, font_id f);
    f32 label_clipped(vec2 pos, f32 max_width, color c, std::string_view s) { return label_clipped(pos, max_width, c, s, current_font()); }

    // what the frame that just ended cost: how many items were submitted and how many of those the clip rectangle
    // culled, the geometry that came out, and how long begin_frame / end_frame took
    [[nodiscard]] const frame_stats& stats() const noexcept { return stats_prev_; }

    // idling ------------------------------------------------------------------
    // a ui that nobody is touching produces byte-identical geometry every frame. these two say so, from end_frame:
    //
    //   ui.end_frame();
    //   if (ui.can_idle()) { /* skip render + present, sleep until the next message */ }
    //   else { renderer.render(ui.render_data()); present(); }
    //
    // `frame_unchanged()` compares this frame's vertices, indices, commands and shapes with the previous frame's
    // (a hash, so it costs a pass over the geometry, ~10 us for a busy frame); `animations_settling()` is true while
    // any hover / press / toggle / custom animation is still moving toward its target, which is the case the hash
    // cannot see coming -- an animation that has one more frame to run produces the same geometry twice in a row
    // near the end, and stopping there would freeze it a pixel short.
    //
    // an overlay drawing into someone else's frame must still re-record and re-render (the game cleared the target);
    // what it can skip is the buffer upload, which the renderers do for it when nothing changed.
    [[nodiscard]] bool frame_unchanged() const noexcept { return frame_unchanged_; }
    [[nodiscard]] bool animations_settling() const noexcept { return anim_settling_; }
    // the two together: nothing to draw that is not already on the screen
    [[nodiscard]] bool can_idle() const noexcept { return frame_unchanged_ && !anim_settling_; }
    // force the next frame to count as changed (a texture was replaced, the theme was edited, the window was resized
    // under a host that keeps its own back buffers)
    void invalidate() noexcept { geometry_hash_ = 0; }

    // widget ids --------------------------------------------------------------
    // two widgets that hash to the same id in the same scope share their animation, active and focus state: the
    // second one steals the first one's press, and neither is obviously wrong on screen. debug builds count them
    // (`stats().id_collisions`) and remember the first, which is almost always a repeated label -- give one of them
    // a "label##suffix" or wrap it in push_id(). release builds do not check, and this returns 0 / an empty view.
    [[nodiscard]] id   id_collision() const noexcept { return collision_id_; }
    [[nodiscard]] std::string_view id_collision_label() const noexcept
    {
        return {collision_label_.data(), collision_label_len_};
    }

    // everything stats() reports, drawn: frame cost, the geometry that came out, how much culling and measure
    // caching saved, the idle state, and -- in red, because they are silent otherwise -- overflows and id
    // collisions. keep `open` and call it every frame; it draws nothing while that is false:
    //
    //   ui.debug_metrics_window(show_metrics);
    //
    // it is an ordinary window, so it is movable, collapsible and themed like the rest. it submits ids of its own
    // under a "##strata_metrics" scope, so it never collides with the application's.
    void debug_metrics_window(bool& open);
    // the live draw commands, one row each (clip rectangle, index count, texture, blur): what merged and what did
    // not, which is the first thing to look at when draw_calls is higher than expected. also keyed off `open`.
    void debug_draw_list_window(bool& open);

    // smooth per-key value chasing `target` at the theme's animation speed (or `speed`, in 1/seconds). starts at `target`
    [[nodiscard]] f32 animate(std::string_view key, f32 target, f32 speed = 0.0f);

    [[nodiscard]] f32  content_width() const noexcept { return layout_.width; }
    // the geometry an app would otherwise hardcode: how much room a scrollbar takes (already subtracted from
    // content_width() when one is showing), and the visible rectangle of the innermost scrolling region being built
    // (the child, otherwise the window body) in logical screen coordinates
    [[nodiscard]] static constexpr f32 scrollbar_width() noexcept { return scrollbar_w; }
    [[nodiscard]] rect content_rect() const noexcept;
    // the press that activated the last tree row landed on its arrow, not on the label: what tree_flags::arrow_only
    // tests internally, so a caller does not have to guess the hit zone
    [[nodiscard]] bool item_arrow_hit() const noexcept { return last_item_arrow_; }
    // the label of the last row / text_ellipsis did not fit and was cut with "..."  (so: add a tooltip)
    [[nodiscard]] bool item_truncated() const noexcept { return last_item_truncated_; }
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

    // disabled items -------------------------------------------------------
    // everything submitted until end_disabled() is drawn faded and does not react: no hover highlight, no press, no
    // keyboard. It still reports item_hovered(), so a tooltip can say why it is disabled. The calls nest, and a
    // disabled scope inside an enabled one stays disabled.
    //   { auto d = ui.disabled_if(selection.empty()); if (ui.button("Delete")) { ... } ui.tooltip("select something first"); }
    void begin_disabled(bool disabled = true) noexcept;
    void end_disabled() noexcept;
    [[nodiscard]] disabled_scope disabled_if(bool disabled = true) noexcept { begin_disabled(disabled); return disabled_scope{*this}; }
    [[nodiscard]] bool item_enabled() const noexcept { return disabled_depth_ == 0; }

    // clipboard ------------------------------------------------------------
    // the hooks the text fields use (set_clipboard), for an app that wants to copy or paste a string of its own
    bool copy_text(std::string_view text) const;
    bool paste_text(std::string& out) const;

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
        f32  grab{};        // scrollbar: where in the thumb it was pressed
        bool collapsed{};
        bool resizable{};
        bool size_set{};
        bool overflow{};    // content taller than the body last frame (scrolling windows)
        f32  capped_h{};    // auto-height window that would reach past the display: the height it is shown at, 0 = not capped
        // docking
        u32  dock{};        // 1 + index of the dock node the window sits in, 0 = floating
        u32  dock_order{};  // position among the tabs of its node
        vec2 float_size{};  // size to go back to when un-docked
        bool docked_now{};  // drawn as a docked window this frame
        f32  ghost{};       // 0..1: see-through while it is carried over a dock target
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
        f32  right{};        // the furthest right edge of anything placed
        f32  gutter{};       // width taken off `width` by push_right_gutter (same_line_right adds it back)
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
        id   seed{};  // the id scope it was submitted in: its parent node, for the recursive open / close
        bool open{};
    };

    struct table_state { // persists across frames
        id                                 key{};
        u64                                last_frame{};
        u32                                columns{};
        std::array<f32, max_table_columns> frac{}; // column widths as fractions of the table width
        std::array<u8, max_table_columns>  order{}; // the column shown at each position (reorderable tables)
        u16                                hidden{}; // a bit per column
        u8                                 press_col1{}; // 1 + the header that was pressed / is being dragged, 0 = none
        u8                                 drag_col1{};
        f32                                press_x{};
        f32                                row_hint{};
        f32                                scroll{};
        f32                                grab{};
        f32                                content_h{};
        bool                               inited{};
    };
    struct table_column {
        std::string_view   label;
        f32                fixed{};
        f32                weight{1.0f};
        table_column_flags flags{};
    };
    struct table_frame { // the table being built this frame
        bool                                      active{};
        table_state*                              state{};
        table_flags                               flags{};
        u32                                       ncols{};
        u32                                       setup_count{};
        std::array<table_column, max_table_columns> cols{};
        std::array<f32, max_table_columns + 1>    col_x{};   // the edges of the visible columns, left to right
        std::array<f32, max_table_columns>        x0{}, x1{}; // where each column is (by declared index; hidden: empty)
        std::array<u8, max_table_columns>         vis{};     // the declared index of each visible column, left to right
        u32                                       nvis{};
        f32                                       frac_total{1.0f}; // the widths of the visible columns add up to this
        u32                                       tree_depth{};
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
        // horizontal counterparts of scroll / grab / content_h / overflow, used by child_flags::horizontal
        f32  scroll_x{};
        f32  grab_x{};
        f32  content_w{};
        bool overflow_x{};
        id   key{};
        u64  last_frame{};
        f32  scroll{};
        f32  grab{};
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

    // docking: the tree of panes and the drag state are in internal::dock_state (src/dock_state.hpp)
    static constexpr u8 no_node = 0xff;

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
        std::string_view link;      // the href of the enclosing <a=...>, empty when this run is not a link
        bool             own_col{}; // a <c=> inside the link set the colour: do not repaint it with the accent
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
    // the id of a widget from its label, in the current id scope: hash_id(label, current_seed()) plus, in debug
    // builds, remembering the label so a collision on this id can name it. every widget that keys itself off a
    // label goes through here.
    [[nodiscard]] id       widget_id(std::string_view label) noexcept;
    // debug builds only: records that `key` was submitted this frame and counts it if it was already seen
    void                   check_id(id key) noexcept;
    [[nodiscard]] anim_slot& anim_for(id key) noexcept;            // finds or creates (and keeps alive)
    [[nodiscard]] anim_slot* anim_find(id key) noexcept;           // finds only
    void                     anim_rehash() noexcept;
    struct press_anim { f32 hover{}; f32 active{}; };
    // idle buttons own no animation state
    [[nodiscard]] press_anim button_anim(id key, const interaction& in) noexcept;
    [[nodiscard]] f32      approach(f32 current, f32 target, f32 speed = 0.0f) const noexcept;
    [[nodiscard]] window_state* window_for(id key, vec2 pos, f32 width) noexcept;
    [[nodiscard]] rect     layout_place(vec2 size) noexcept;
    // a row whose rectangle is outside the clip rectangle does no work at all: no hit test, no animation slot, no
    // measuring, no geometry. the layout has already been advanced when this is asked, so the scrollbar range, the
    // tree nesting and the open / closed state are the same either way
    [[nodiscard]] bool     item_culled(const rect& r) noexcept;
    // bookkeeping a culled row still owes its caller: it is the "last item", and nothing happened to it
    void                   note_culled_item(id key, const rect& r) noexcept;
    // plain text and other non-interactive content: "the last item" for the questions that follow, no press
    void                   note_passive_item(id key, const rect& r) noexcept;
    [[nodiscard]] vec2     measure_cached(font_id f, std::string_view s) noexcept;
    // nav: every row of an open nav scope records itself here, culled or not
    void                   nav_record(id key, const rect& r, u32 depth, bool node, bool open) noexcept;
    [[nodiscard]] bool     nav_take(id key) noexcept;   // the cursor is on this row and Enter was pressed for it
    [[nodiscard]] bool     nav_is_cursor(id key) const noexcept;
    void                   nav_click(id key) noexcept;  // a click moves the cursor
    // the accessory strip of the row just submitted: its rectangle, and false when there is no row to attach to
    [[nodiscard]] bool     accessory_slot(f32 width, rect& out) noexcept;
    // a row that accessories may be hung on (see row_anchor_)
    void                   note_row_anchor(id key, const rect& r) noexcept
    {
        row_anchor_      = key;
        row_anchor_rect_ = r;
    }
    void                   allow_item_overlap_at(id key, const rect& r) noexcept;
    // the scroll offset / view of the innermost scrolling region being built, or nullptr when nothing scrolls
    [[nodiscard]] f32*     scroll_slot(rect& view) noexcept;
    void                   scroll_reveal_rect(const rect& item, bool center) noexcept;
    // tree open state, with set_next_item_open / the bulk open-close applied
    [[nodiscard]] bool&    tree_open_resolved(id key, bool default_open, bool& recursive_out) noexcept;
    void                   tree_set_recursive(id key, bool open) noexcept;
    void                   tree_set_bulk(id seed, bool open) noexcept;
    void                   tree_open_set(id key, bool open) noexcept;
    [[nodiscard]] id       tree_seed_of(id key) const noexcept;
    [[nodiscard]] bool     tree_under(id node, id root) const noexcept;
    [[nodiscard]] bool     id_in_scope(id seed) const noexcept;
    void                   push_id_value(id key) noexcept; // an id that is already hashed (a row's own key)
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
    void end_popup_at();
    bool picker_body(id key, color& c, color_flags flags);
    bool hotkey_field(std::string_view label, u32& key_code, key_chord* chord);
    // a scrollbar thumb that follows the pointer: the new scroll offset while it is pressed / dragged. `grab` remembers where
    // in the thumb the press was (a press beside the thumb puts its middle under the pointer), `travel` is how far the
    // thumb can move along the track
    [[nodiscard]] f32 thumb_drag(const interaction& in, f32& grab, f32 thumb_y, f32 thumb_h, f32 track_top, f32 travel,
                                 f32 max_scroll, f32 scroll) const noexcept;
    [[nodiscard]] f32 thumb_drag_x(const interaction& in, f32& grab, f32 thumb_x, f32 thumb_w, f32 track_left,
                                   f32 travel, f32 max_scroll, f32 scroll) const noexcept;
    [[nodiscard]] f32 thumb_drag_along(f32 along, const interaction& in, f32& grab, f32 thumb_lo, f32 thumb_len,
                                       f32 track_lo, f32 travel, f32 max_scroll, f32 scroll) const noexcept;
    // the innermost open child region with child_flags::horizontal, which owns the x scroll offset
    [[nodiscard]] child_state* horizontal_child() const noexcept;
    void apply_input_mask();
    bool edit_indent_lines(bool unindent, int tab_size);
    void picker_field(id key, const rect& box, const interaction& in, bool open, std::string_view text, int icon);
    bool pick_cell(id key, const rect& r, std::string_view text, bool selected, bool faded, bool marked);
    bool calendar_body(date& value, bool close_on_pick);
    bool time_body(time_of_day& value, bool seconds);
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
    [[nodiscard]] internal::dock_target dock_pick(vec2 pointer, u8 exclude_leaf = no_node, vec2 want = {}) const noexcept; // want: the size the window has
    void dock_attach(window_state& w, u8 space, u8 node, dock_zone zone, bool outer, f32 size = 0.0f, int tab = -1) noexcept;
    [[nodiscard]] f32 dock_tab_width(const window_state& w) const noexcept;
    [[nodiscard]] u32 dock_leaf_tabs(u8 leaf, std::array<u8, max_windows>& out, bool shown_only) const noexcept; // indices into windows_, in tab order
    void dock_move_tab(window_state& w, u32 index) noexcept;                                  // to that place among its neighbours
    [[nodiscard]] u32 dock_guides(const internal::dock_space& sp, u8 leaf, std::array<internal::dock_guide, 9>& out) const noexcept;
    [[nodiscard]] bool dock_double_click(id key) noexcept;                                    // the second press of a quick pair on `key`
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
    f32  wheel_x_{};   // horizontal wheel this frame (see input_state::wheel_x)
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
    bool        wheel_x_consumed_{};
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
    bool          mouse_middle_down_{};
    bool          mouse_middle_pressed_{};
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
    bool          dock_animation_{true};

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

    // table layouts loaded before the table was drawn: applied once when it is
    std::vector<std::pair<id, std::string>> table_pending_;

    // tab bars: scratch for the cells of the bar being built, and the tab that is being dragged to a new place
    std::vector<rect> tab_cells_;
    std::vector<f32>  tab_emph_;
    id                tabdrag_tab_{};
    id                tabdrag_cand_{};
    f32               tabdrag_grab_{};
    f32               tabdrag_press_x_{};

    // input masks and code fields: what the text field being built has to do beyond plain editing
    std::string_view edit_mask_;         // input_masked: the mask (only while the call runs)
    id               focus_request_{};   // a text field that should take the keyboard when it is drawn next
    struct code_mode {
        bool                                          on{};
        code_flags                                    flags{};
        int                                           tab_size{4};
        std::span<const std::pair<u32, u32>>          marks{}; // find matches (start, length), sorted
        int                                           mark_active{-1};
        std::size_t                                   goto_offset{~std::size_t{0}};
    };
    code_mode code_;
    struct code_state { // per code field, across frames
        id          key{};
        u64         last_frame{};
        bool        find_open{};
        bool        replace_open{};
        bool        match_case{};
        bool        open_request{}; // code_find(): give the find field the keyboard
        int         active{-1};
        int         want_line{};
        std::string find;
        std::string replace;
    };
    std::vector<code_state>                code_states_;
    std::vector<std::pair<u32, u32>>       code_marks_;
    [[nodiscard]] code_state& code_state_for(id key);

    // date picker: the month being shown, and the popup it belongs to
    id  cal_key_{};
    i32 cal_year_{};
    i32 cal_month_{};

    // generic popups (open_popup): where it opened from and how big its content was last frame
    rect gpopup_anchor_{};
    vec2 gpopup_size_{};
    id   gpopup_key_{};       // the popup gpopup_size_ was measured for
    bool gpopup_hidden_{};    // this frame's popup is only being measured

    // drag and drop
    bool            dd_active_{};     // a payload is being dragged
    bool            dd_cancelled_{};  // Esc: ignored until the button is let go
    id              dd_source_{};     // the widget it was picked up from
    id              dd_candidate_{};  // a pressed widget that becomes a drag source once the pointer has moved
    vec2            dd_press_pos_{};
    std::string     dd_type_;
    std::vector<u8> dd_data_;
    vec2            dd_size_{};       // the preview panel, measured last frame
    bool            dd_hidden_{};
    bool            dd_saved_overlay_{};
    u32             dd_prev_owner_{};
    layout_state    dd_saved_layout_{};
    rect            last_item_rect_{};

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
    std::unique_ptr<internal::dock_state>  dock_;
    bool                                   dock_chrome_cur_{};
    bool                                   dock_chrome_prev_{};
    bool                                   hovered_docked_cur_{};
    bool                                   hovered_docked_prev_{};
    bool                                   window_faded_{};   // the window being built pushed an alpha that end_window pops
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

    // per-frame instrumentation: the frame being built, and the one stats() reports
    frame_stats stats_cur_{};
    frame_stats stats_prev_{};
    f64         begin_frame_ms_{};   // how long the last begin_frame took
    f64         frame_clock_{};      // when it started

    // idling: a hash of everything the renderer sees, so an untouched ui can be recognised and skipped.
    // 0 is "no previous frame", which invalidate() writes to force the next one to count as changed.
    u64  geometry_hash_{};
    bool frame_unchanged_{};
    bool anim_settling_{};
    // set by approach() whenever it returned something short of its target: the frame after this one will look
    // different even if nothing is touched. mutable because approach() is const.
    mutable bool anim_moved_{};

    // rich-text links. the hrefs are views into the caller's markup, so what is reported is copied out: the strings
    // are reused every frame and never reallocate in steady state. `rich_links_live_` is set only by rich_text /
    // rich_text_wrapped -- a link inside a button's caption is drawn but must not steal the button's click.
    std::string rich_clicked_;
    std::string rich_hovered_;
    bool        rich_links_live_{};

    // debug-only duplicate-id detection. a direct-mapped table of the ids submitted this frame: a hit whose id
    // matches is a collision. direct-mapped (not a set) so it cannot allocate or grow mid-frame; a collision between
    // two ids that land in different slots is missed, which is the usual trade for a fixed table -- the common case
    // (the very same label twice) always lands in the same slot and is always caught.
    static constexpr u32 id_seen_size = 2048; // power of two
    std::vector<id>       id_seen_;
    id                    collision_id_{};
    std::array<char, 64>  collision_label_{};
    u32                   collision_label_len_{};
    // ... and the label each id came from, so the report can name it. same direct-mapped shape.
    struct id_label_slot {
        id                   key{};
        std::array<char, 48> text{};
        u8                   len{};
    };
    static constexpr u32       id_label_size = 1024; // power of two
    std::vector<id_label_slot> id_labels_;

    // label_size() cache: a direct-mapped table of (font, string) -> size, thrown away when the atlas changes.
    // rich (markup) labels are never cached: their size depends on the style stack.
    struct measure_slot {
        u64  key{};   // 0 = empty
        vec2 size{};
    };
    static constexpr u32 measure_cache_size = 4096; // power of two
    std::vector<measure_slot> measure_cache_;
    u32                       measure_cache_gen_{};

    // items submitted on top of an earlier one that called allow_item_overlap()
    id   overlap_key_{};        // the item that offered its rectangle, this frame
    rect overlap_rect_{};
    bool overlap_stolen_{};     // a later item took the press from it
    id   overlap_taken_cur_{};  // ... or was merely hovered over it: its hover is suppressed next frame
    id   overlap_taken_prev_{};

    // set_next_item_open: 0 none, 1 open, 2 close, 3 open recursively, 4 close recursively
    u8   next_open_{};
    // open_all_tree_nodes / close_all_tree_nodes / a recursive arrow click: 0 none, 1 open, 2 close
    u8   tree_bulk_{};
    id   tree_bulk_seed_{};
    u32  tree_bulk_frames_{};   // a bulk open also applies to nodes that only appear as their parents open

    // keyboard navigation of a list or tree (nav_begin / nav_end)
    struct nav_item {
        id   key{};
        rect bounds{};
        u32  depth{};
        bool node{};
        bool open{};
    };
    struct nav_state {
        id                    scope{};      // the scope being built, 0 between nav_end and the next nav_begin
        id                    scope_key{};  // which list the cursor belongs to (it starts over for another one)
        id                    cursor{};     // the row the cursor is on, kept across frames
        id                    activate_pending{}; // a row Enter picked last frame, reported as a press on this one
        bool                  active{};     // the scope has the keyboard (no text field has it)
        std::vector<nav_item> items;        // every row submitted this frame, culled ones included
    };
    nav_state nav_{};
    // right gutters (push_right_gutter), so they nest
    static constexpr u32 max_gutter_depth = 8;
    std::array<f32, max_gutter_depth> gutter_stack_{};
    u32                               gutter_depth_{};

    // disabled scopes (begin_disabled): the depth, whether the outermost one pushed the fade, and which of the
    // open scopes actually counted, so end_disabled() stays balanced when enabled and disabled ones nest
    static constexpr u32 max_disabled_depth = 16;
    u32                                disabled_depth_{};
    bool                               disabled_alpha_{};
    std::array<bool, max_disabled_depth> disabled_stack_{};
    u32                                disabled_count_{};

    // every key held this frame, straight from input_state
    std::array<u8, 32> keys_held_{};
    // the window the keyboard belongs to: the last one pressed in, docked ones included (they do not restack, so
    // focused_window_, which is the topmost, cannot answer this)
    id   key_window_{};

    // row accessories: the row they hang on -- which is the last selectable / tree row / custom_item, not simply the
    // last item, because an accessory is an item itself and would otherwise become the anchor for the next one
    f32  next_gutter_{};
    id   row_anchor_{};
    rect row_anchor_rect_{};
    id   accessory_row_{};   // the row the accessories laid out so far belong to
    f32  accessory_x_{};     // their left edge, which each one moves further left
    rect accessory_row_rect_{};

    // the self-contained confirmation (ask_confirm / confirm)
    id          confirm_key_{};
    u64         confirm_data_{};
    std::string confirm_message_;
    int         confirm_answer_{};   // set on the frame a button is pressed, read once by confirm()
    id          confirm_answer_key_{};
    id          confirm_open_{};     // the confirm() whose modal is up
    bool        confirm_pending_{};  // ask_confirm() ran; the next confirm() with that id opens the modal
    bool        confirm_remember_{}; // the state of the "don't ask again" checkbox while the dialog is open

    // combo_filtered: the text of the search field in the open popup, what is left after it, and the row the
    // keyboard is on
    std::string      combo_filter_;
    int              combo_filter_hover_{};
    bool             combo_filter_focus_{};
    std::vector<u32> combo_filter_hits_;

    static constexpr f32 scrollbar_w = 10.0f;

    // tooltips: what the pointer rests on
    id   last_item_key_{};
    bool last_item_hovered_{};
    bool last_item_focused_{};  // ... and it is where the nav cursor is
    bool last_item_pressed_{};
    bool last_item_double_{};   // the press that ended on it was the second of a pair
    bool last_item_arrow_{};    // ... and it landed on a tree row's arrow
    bool last_item_truncated_{}; // the last label did not fit and was cut
    // double clicks on widgets, tracked apart from register_click() (whose 1 / 2 / 3 run belongs to text fields)
    id   item_click_key_{};
    f64  item_click_time_{-10.0};
    vec2 item_click_pos_{};
    bool item_dbl_pending_{};
    id   hover_key_cur_{};
    id   hover_key_prev_{};
    f32  hover_time_{};

    // hotkey binding
    u32  pressed_key_{};
    id   hotkey_capture_{};
    bool hotkey_seen_{};
    // hotkey_sequence(): the steps captured so far of the field currently capturing (hotkey_capture_ ensures only one)
    std::array<key_chord, key_sequence::max_steps> seq_edit_capture_{};
    u8   seq_edit_count_{};
    f64  seq_edit_deadline_{};
    // sequence_pressed(): the prefix of steps matched so far, across whichever sequences are asked about each frame;
    // mutable so the method that only reads a chord you keep can stay const, like chord_pressed
    mutable std::array<key_chord, key_sequence::max_steps - 1> seq_pending_{};
    mutable u8   seq_pending_count_{};
    mutable f64  seq_pending_time_{};
    mutable bool seq_pending_touched_{}; // some call advanced or completed the pending prefix this frame (see begin_frame)

    std::array<window_state, max_windows> windows_{};
    std::vector<anim_slot>                anims_;     // open addressing, power-of-two size, grows on demand
    u32                                   anim_used_{}; // occupied slots, live or stale
};


// a loop over the rows of a long list that only submits the ones in view; the rest are reserved as empty space, so the
// scrollbar and the layout are those of the whole list. use it inside a scrolling area (a fixed-height window, a child, a
// table with a height): every row must have the same height.
//   strata::list_clipper clip(ui, rows.size(), ui.frame_height());
//   while (clip.step()) { for (int i = clip.begin(); i < clip.end(); ++i) { draw_row(rows[i]); } }
class list_clipper {
public:
    list_clipper(context& ui, std::size_t count, f32 item_height = 0.0f) noexcept
        : ui_{&ui}, count_{static_cast<int>(count)}, height_{item_height} {}
    ~list_clipper()
    {
        if (state_ == 1) { ui_->list_clip_end(count_, height_, end_); } // the loop was left early
    }
    list_clipper(const list_clipper&)            = delete;
    list_clipper& operator=(const list_clipper&) = delete;

    // true once (the rows begin() .. end() are to be submitted), then false
    [[nodiscard]] bool step()
    {
        if (state_ == 0) {
            ui_->list_clip_begin(count_, height_, begin_, end_);
            state_ = 1;
            return true;
        }
        if (state_ == 1) {
            ui_->list_clip_end(count_, height_, end_);
            state_ = 2;
        }
        return false;
    }
    [[nodiscard]] int begin() const noexcept { return begin_; }
    [[nodiscard]] int end() const noexcept { return end_; }

private:
    context* ui_;
    int      count_;
    f32      height_;
    int      begin_{};
    int      end_{};
    int      state_{};
};

} // namespace strata
