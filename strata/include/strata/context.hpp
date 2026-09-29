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
    bool alt{};   // text fields ignore Alt + key (a shortcut, not caret movement)
};

// next_wake_seconds() result when only input can change anything (finite: /fp:fast may mis-compare infinities)
inline constexpr f64 no_deadline = std::numeric_limits<f64>::max();

// one key press (virtual-key code) or extra mouse button, with its modifiers
struct key_press {
    u32  key{};
    bool ctrl{};
    bool shift{};
    bool alt{};
};

inline constexpr u32 max_key_events  = 16;
inline constexpr u32 max_key_presses = 16;
// a confirmed IME sentence arrives in one frame: 1 KiB ~ 340 CJK characters
inline constexpr u32 max_typed_bytes = 1024;

// mouse_pos and display_size are physical pixels; the context divides by its scale
struct input_state {
    vec2                mouse_pos{};
    std::array<bool, 3> mouse_down{};
    f32                 wheel{};
    // horizontal wheel (tilt / trackpad, WM_MOUSEHWHEEL). positive = right, opposite to `wheel`'s convention
    f32                 wheel_x{};
    // seconds since the previous frame. animations take at most 0.1 s of it per frame; timers (toasts, tooltip delay,
    // caret blink) take all of it, so a host sleeping until next_wake_seconds() wakes at the deadline
    f32                 delta_time = 1.0f / 60.0f;
    vec2                display_size{};
    // system settings (win32_platform fills them): caret on/off time (0 = no blink) and max double-click interval
    f32                 caret_blink_time  = 0.53f;
    f32                 double_click_time = 0.35f;
    // lines per wheel notch (Windows default 3); <= 0 means a screenful per notch
    f32                 wheel_lines       = 3.0f;

    // this frame's key presses (auto-repeat included) and typed utf-8 text
    std::array<key_event, max_key_events> keys{};
    u32                                   key_count{};
    std::array<char, max_typed_bytes>     typed{};
    u32                                   typed_len{};
    // every key / extra mouse button (4 middle, 5 / 6 side) pressed since the last frame, in order, with modifiers.
    // the context consumes one per frame so shortcuts pressed between two slow frames all fire. win32_platform fills it.
    std::array<key_press, max_key_presses> presses{};
    u32                                    press_count{};
    // single-key fallback for simple hosts: used when press_count is 0, with the modifiers below
    u32                                   pressed_key{};
    // IME composition (utf-8) and its caret in bytes. a state, repeated every frame until confirmed into `typed`
    std::array<char, 256>                 ime{};
    u32                                   ime_len{};
    u32                                   ime_cursor{};
    // modifiers held now (drag widgets: shift = fine, alt = coarse)
    bool                                  ctrl{};
    bool                                  shift{};
    bool                                  alt{};
    // every held key, one bit per virtual-key code. zeroed if the host cannot fill it, in which case key_down() is
    // always false; key_pressed(vk) works either way via `pressed_key`
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

// readable name of a virtual-key code ("A", "F5", "Space", "Mouse 4" ...); "None" for 0
[[nodiscard]] std::string_view key_name(u32 virtual_key) noexcept;

// a key (virtual-key code or extra mouse button) with the exact modifiers held with it
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
// the inverse, also accepts accelerator() syntax ("ctrl + s"). "" = unbound; false (`out` untouched) on an unknown part
[[nodiscard]] bool chord_from_string(std::string_view text, key_chord& out) noexcept;

// max seconds between the steps of a key_sequence (sequence_pressed, hotkey_sequence)
inline constexpr f64 key_sequence_timeout = 1.5;

// up to max_steps chords pressed in turn ("Ctrl+K" then "Ctrl+S"), each within key_sequence_timeout of the last.
// a key_chord converts to a one-step sequence
struct key_sequence {
    static constexpr u32 max_steps = 3;

    std::array<key_chord, max_steps> steps{};
    u8                                count{};

    constexpr key_sequence() noexcept = default;
    constexpr key_sequence(const key_chord& c) noexcept : steps{c}, count{c.bound() ? u8{1} : u8{0}} {}

    [[nodiscard]] constexpr bool bound() const noexcept { return count > 0 && steps[0].bound(); }
    friend constexpr bool operator==(const key_sequence&, const key_sequence&) noexcept = default;
};

// "Ctrl+K, Ctrl+S"; empty for an unbound sequence
[[nodiscard]] std::string sequence_to_string(const key_sequence& seq);
// the inverse. "" = unbound; false (`out` untouched) if a step does not parse or there are too many
[[nodiscard]] bool sequence_from_string(std::string_view text, key_sequence& out) noexcept;

// text-field copy / paste; win32_platform::clipboard() provides them
struct clipboard_hooks {
    void (*set)(void* user, std::string_view text) noexcept = nullptr;
    bool (*get)(void* user, std::string& out) noexcept      = nullptr;
    void* user                                              = nullptr;
};

// silent failures (the ui drew something other than asked), reported to the app instead of only stderr.
// each distinct problem is reported once per context.
enum class diagnostic_kind : u8 {
    limit,         // a fixed-size table / stack ran out (max_windows, push_id depth ...); the excess was dropped
    id_collision,  // two widgets used one id in a frame (debug builds): use "##suffix" / push_id
    draw_overflow, // draw list out of vertices / indices / commands (raise draw_list_limits), or clip / alpha nesting
};

struct diagnostic {
    diagnostic_kind  kind{};
    std::string_view message; // one line, readable as it is
};

// called on the ui thread inside the offending frame; `message` is valid only during the call
struct diagnostics_hook {
    void (*report)(void* user, const diagnostic& d) noexcept = nullptr;
    void* user                                               = nullptr;
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
    f32  tooltip_delay_s = 0.4f; // hover time before a tooltip shows
    // thickens glyph edges in proportion to text lightness (light-on-dark reads thin). 0 = as rasterised, 1 = strong
    f32  text_contrast = 0.0f;
    // wheel scroll multiplier on top of input_state::wheel_lines; 0 disables wheel scrolling
    f32  scroll_speed  = 1.0f;
    vec2 frame_padding{10.0f, 5.0f};
    f32  blur_radius   = 18.0f;   // acrylic blur radius (logical pixels)
    f32  acrylic_alpha = 0.62f;   // acrylic windows multiply window_bg alpha by this
    f32  acrylic_noise = 0.035f;  // fine grain over acrylic panels (0 .. 1)
    f32  acrylic_saturation = 1.0f; // acrylic saturation (0 grey, 1 as is, > 1 boosted)
    f32  acrylic_brightness = 1.0f; // ... and how bright (1 as is)
    f32  popup_acrylic = 0.0f;    // popups / tooltips / toasts: 0 = opaque, 1 = acrylic

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
    // semantic colours (toasts, badges, log levels, validation); light themes want darker ones
    color success       = color::from_hex(0x4ade80ff);
    color warning       = color::from_hex(0xffb454ff);
    color error         = color::from_hex(0xff5d6cff);
    // default chart series colours in turn; alpha 0 = the accent
    std::array<color, 6> series{color{0, 0, 0, 0}, color::from_hex(0x19c2b4ff), color::from_hex(0xffb454ff),
                                color::from_hex(0xf0568fff), color::from_hex(0xa78bfaff), color::from_hex(0x7bc74dff)};
};

// style members push_color / push_var can override
enum class style_color : u8 {
    window_bg, title_bg, border, widget_bg, widget_hover, widget_active, widget_border,
    accent, accent_hover, text, text_dim, shadow, modal_dim,
    success, warning, error,
    count_
};

enum class style_var : u8 {
    padding, item_spacing, rounding, border_width, shadow_blur, gradient, anim_speed,
    frame_padding_x, frame_padding_y, blur_radius, acrylic_alpha, acrylic_noise,
    acrylic_saturation, acrylic_brightness, popup_acrylic, tooltip_delay, text_contrast, scroll_speed,
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
    diagnostics_hook             diagnostics{};   // none: stderr and debugger output
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

// what the frame that just ended cost (context::stats(), filled at end_frame)
struct frame_stats {
    u32 items_submitted{};  // rows / widgets through the cullable item path
    u32 items_culled{};     // ... of those, culled by the clip rectangle
    u32 vertices{};
    u32 indices{};
    u32 draw_calls{};
    u32 text_measures{};    // label_size() calls that had to walk the string
    u32 measure_hits{};     // ... and the ones the measurement cache answered
    u32 anim_slots_used{};  // occupied animation slots (live and stale)
    u32 anim_slots_total{}; // its capacity
    // silent failures: dropped geometry (raise draw_list_limits), clip / alpha stack overflow, id collisions
    u32 draw_overflow{};       // 1 if vertex / index / command / shape arrays ran out
    u32 clip_overflows{};
    u32 alpha_overflows{};
    u32 id_collisions{};    // ids submitted twice this frame (debug builds only)
    u32 limits_hit{};       // fixed-size table / stack overflows this frame
    // nothing the renderer sees changed / animations still moving; see frame_unchanged()
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
    no_wrap             = 8, // input_multiline: scroll sideways instead of wrapping
    no_frame            = 16, // input_multiline: no background, border or padding
    auto_height         = 32, // input_multiline: grow to fit the text, never scroll
    reveal              = 64, // with password: eye button that shows the text while toggled
    clear_button        = 128, // x button that empties a non-empty field
};

[[nodiscard]] constexpr input_flags operator|(input_flags a, input_flags b) noexcept
{
    return static_cast<input_flags>(static_cast<u16>(a) | static_cast<u16>(b));
}

// input_code extras
enum class code_flags : u8 {
    none           = 0,
    line_numbers   = 1,  // a gutter with the line numbers
    highlight_line = 2,  // a faint bar behind the line the caret is on
    bracket_match  = 4,  // box the bracket at the caret and its pair
    auto_indent    = 8,  // Enter keeps indentation (+1 after an opening bracket), a typed } dedents
    find_replace   = 16, // Ctrl+F / Ctrl+H find / replace bar
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
    resizable     = 1,  // resize by the right / bottom edge or corner
    no_title_bar  = 2,  // no title bar or collapse arrow; move with drag_by_body
    no_collapse   = 4,  // keep the title bar but hide the collapse arrow
    no_move       = 8,  // the window cannot be dragged
    no_background = 16, // no fill, border or shadow
    drag_by_body  = 32, // dragging any empty part of the window moves it
    dockable      = 64, // can dock into dock_area(); needs a title bar
    acrylic       = 128, // blurred backdrop shows through the translucent background
};

[[nodiscard]] constexpr window_flags operator|(window_flags a, window_flags b) noexcept
{
    return static_cast<window_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

// pointer shape for hosts that can set it (win32_platform::set_cursor). `hand` = clickable (links),
// `not_allowed` = disabled item, `resize_nesw` = bottom-left grip
enum class cursor_kind : u8 { arrow, text, hand, not_allowed, resize_ew, resize_ns, resize_nwse, resize_nesw };

enum class child_flags : u8 {
    none         = 0,
    frame        = 1, // draw a rounded background and border
    no_padding   = 2,
    no_scrollbar = 4, // clips and wheel-scrolls, no scrollbar
    acrylic      = 8, // frosted background (see window_flags::acrylic)
    // wider content scrolls sideways (scrollbar, tilt wheel, Shift + wheel) instead of being clipped. full-width widgets
    // still fit the visible width; only fixed-width content overflows. see scroll_x() / set_scroll_x()
    horizontal   = 16,
};

[[nodiscard]] constexpr child_flags operator|(child_flags a, child_flags b) noexcept
{
    return static_cast<child_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

enum class tab_strip_flags : u8 {
    none       = 0,
    icons_only = 1, // icon strip, labels as tooltips
};

enum class color_flags : u8 {
    none     = 0,
    no_alpha = 1, // no alpha bar, alpha kept
};

enum class tree_flags : u8 {
    none         = 0,
    default_open = 1, // open the first time it is seen
    selected     = 2, // highlight the row
    arrow_only   = 4, // only the arrow toggles; read clicks with item_pressed()
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
    hideable    = 16, // header right-click menu shows / hides columns
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
    default_hidden = 1, // hidden until shown (needs table_flags::hideable)
    no_hide        = 2, // the menu cannot hide it
    no_reorder     = 4, // cannot be dragged, others do not move past it
};

[[nodiscard]] constexpr table_column_flags operator|(table_column_flags a, table_column_flags b) noexcept
{
    return static_cast<table_column_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

// modal windows: what closes them besides the code
enum class modal_flags : u8 {
    none            = 0,
    esc_closes      = 1, // Esc closes (unless a text field or popup inside has focus)
    backdrop_closes = 2, // a click on the dimmed area around it closes it
    no_title_bar    = 4,
    resizable       = 8,
};

[[nodiscard]] constexpr modal_flags operator|(modal_flags a, modal_flags b) noexcept
{
    return static_cast<modal_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

// confirm() extras
struct confirm_options {
    std::string_view title;            // empty: "Confirm"
    // "don't ask again" checkbox. while *remember is true the dialog never opens and confirm() returns `remembered`
    bool*            remember{nullptr};
    std::string_view remember_label{"Don't ask again"};
    int              remembered{1};    // what confirm() returns while *remember is true (1 = first button)
    int              danger{0};        // 1..n: that button is drawn in the warning colour
};

enum class toast_kind : u8 { info, success, warning, error };

// toast / badge colour: success / warning / error, accent for info
[[nodiscard]] color kind_color(toast_kind kind, const style& theme) noexcept;

// handle to a live toast (progress, close, actions); 0 = none
using toast_handle = u64;
inline constexpr f32 toast_no_progress = -1.0f;
inline constexpr f32 toast_busy        = -2.0f; // an endless bar for work of unknown length

struct toast_options {
    std::string_view                  title;      // empty: the kind's name (or nothing for info)
    std::string_view                  text;
    toast_kind                        kind{toast_kind::info};
    f32                               seconds{3.5f}; // <= 0: until closed (progress toasts close 2 s after completing)
    std::span<const std::string_view> actions;    // buttons; pressing one closes the toast (see toast_action)
    f32                               progress{toast_no_progress}; // 0..1 fills a bar; toast_busy = indeterminate
};
enum class screen_corner : u8 { top_right, top_left, bottom_right, bottom_left };

// a pill label with optional close button / selected state (see context::chip)
struct chip_options {
    bool             closable{false};   // x button; reports `closed`, removal is up to the caller
    bool*            selected{nullptr}; // click toggles `selected` (filter / tag chips)
    color            tint{0, 0, 0, 0};  // alpha 0: the theme's accent
    std::string_view icon;              // a glyph drawn with `icon_font` before the label
    font_id          icon_font{0};
};

struct chip_result {
    bool clicked{};
    bool closed{};
};

// drop_target() result: a matching payload is over the widget (`hovering`) or was released on it (`dropped`)
struct drop_result {
    bool                 hovering{};
    bool                 dropped{};
    vec2                 local{};  // pointer in the widget, 0..1 per axis
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
    no_highlight = 1, // no highlight while a payload hovers (draw your own marker)
};

enum class log_view_flags : u8 {
    none       = 0,
    no_toolbar = 1, // just the lines
};

// automatic plot value range
inline constexpr f32 plot_auto = std::numeric_limits<f32>::quiet_NaN();

enum class plot_kind : u8 { lines, histogram };

struct plot_series {
    std::string_view    name;
    std::span<const f32> values;
    color               col{0, 0, 0, 0}; // alpha 0 = theme series colour (`{}` would be opaque black)
};

// a styled byte range [start, end) of a text field (font, colour, style); see context::input_spans
struct text_span {
    u32        start{};
    u32        end{};
    font_id    font{0};
    color      col{0, 0, 0, 0};              // alpha 0: the field's own text color
    text_flags style{text_flags::none};
};

// one chart axis (see context::plot with plot_options)
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
    bool      ticks{true};              // nice ticks, labels and grid lines
    bool      fill{false};              // line series: fill fading toward the axis
    f32       fill_alpha{0.28f};
    bool      zoom_pan{false};          // wheel zooms x (Ctrl: y), drag pans, double-click resets
};

// menu row options (see context::menu_item)
struct menu_item_options {
    std::string_view shortcut;            // right-aligned hint, e.g. "Ctrl+O" (accelerator() makes it work)
    std::string_view icon;                // glyph / short text in the icon column, drawn with `icon_font`
    font_id          icon_font{0};        // e.g. the icon font's id
    color            icon_color{0, 0, 0, 0}; // alpha 0: the text color
    texture_id       image{0};            // texture instead of a glyph
    bool             selected{false};     // check mark (with an icon: icon highlighted)
    bool             enabled{true};
    bool             keep_open{false};    // clicking does not close the menus
};

// dock target: `center` joins as a tab, the others split
enum class dock_zone : u8 { center, left, right, top, bottom };

// side of a region for an edge dock
enum class dock_side : u8 { left, right, top, bottom };

struct tab_desc {
    std::string_view label;
    std::string_view icon; // optional; drawn with tab_bar's icon font
    // stable identity when the caption changes (dirty dot, count ...): keeps selection, place and drag state.
    // empty = the label is the identity
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

// what happened in a tab bar this frame; the caller applies it (apply_tab_events). labels must be unique
struct tab_events {
    bool changed{};      // `selected` changed
    int  closed{-1};     // the tab whose x was pressed
    int  moved_from{-1}; // tab dragged past a neighbour, and its new index
    int  moved_to{-1};
    bool add{};          // the "+" was pressed
};

// applies tab_events to a tab list and the selected index
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

// list selection with the usual modifiers: click selects one, Ctrl toggles, Shift selects a range from the anchor.
// indices are sorted; pass it to context::selection_click() with the clicked row.
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
    // Shift-range anchor, -1 if none
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
    // adds [lo, hi]; the anchor stays put
    void add_range(int lo, int hi)
    {
        if (lo > hi) { std::swap(lo, hi); }
        for (int i = lo; i <= hi; ++i) { add(i); }
    }
    // drops indices outside [0, count); call when the list shrinks
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
class config;

namespace internal { // (src/dock_state.hpp)
struct dock_state;
struct dock_target;
struct dock_guide;
struct dock_space;
} // namespace internal

namespace internal { struct anim_slot; } // (src/core/animation.hpp)
namespace internal { struct window_state; } // (src/core/window_registry.hpp)
namespace internal { struct layout_state; } // (src/core/layout_state.hpp)
namespace internal { class popup_stack; } // (src/popup_state.hpp)
namespace internal { struct toast_entry; } // (src/toast_state.hpp)
namespace internal { struct menu_level; struct menu_frame; } // (src/menu_state.hpp)
namespace internal { struct code_mode; struct code_state; } // (src/code_state.hpp)

// false when the window is collapsed; end_window always runs
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

// calls end_menu() / end_popup_menu() / end_main_menu_bar() on scope exit
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

// nav_end() on scope exit
class nav_scope {
public:
    explicit nav_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~nav_scope();

    nav_scope(const nav_scope&)            = delete;
    nav_scope& operator=(const nav_scope&) = delete;

private:
    context* ctx_;
};

// end_disabled() on scope exit
class disabled_scope {
public:
    explicit disabled_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~disabled_scope();

    disabled_scope(const disabled_scope&)            = delete;
    disabled_scope& operator=(const disabled_scope&) = delete;

private:
    context* ctx_;
};

// pop_right_gutter() on scope exit
class gutter_scope {
public:
    explicit gutter_scope(context& ctx) noexcept : ctx_{&ctx} {}
    ~gutter_scope();

    gutter_scope(const gutter_scope&)            = delete;
    gutter_scope& operator=(const gutter_scope&) = delete;

private:
    context* ctx_;
};

// end_popup() on scope exit; false while closed
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

// end_drag_source() on scope exit; true while dragging
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
    // zeroes the temporary's typed-text bytes once read: begin_frame(platform.new_frame())
    void begin_frame(input_state&& input);
    void end_frame();
    [[nodiscard]] draw_data render_data() const noexcept;

    // pointer is over ui or dragging: the host should ignore mouse input
    [[nodiscard]] bool want_capture_mouse() const noexcept;
    // a text field has focus: the host should not fire hotkeys
    [[nodiscard]] bool want_text_input() const noexcept;
    // a popup is open (it handles Esc)
    [[nodiscard]] bool popup_open() const noexcept;
    // wanted pointer shape; apply with win32_platform::set_cursor
    [[nodiscard]] cursor_kind cursor() const noexcept;
    // Enter pressed in a text field this frame
    [[nodiscard]] bool input_submitted() const noexcept;

    void set_clipboard(const clipboard_hooks& hooks) noexcept;
    // diagnostics sink; a hook with no `report` restores stderr / debugger output
    void set_diagnostics(const diagnostics_hook& hook) noexcept;

    // dpi / ui scale -------------------------------------------------------
    // layout is in logical pixels (`scale` physical each); input and draw output are physical.
    [[nodiscard]] f32 scale() const noexcept;
    // scale 0.5 .. 4. rebuilds the font atlas (slow); then re-upload (renderer.update_atlas(ui.font())) and call
    // release_font_pixels(). font_generation() changes. on error nothing changes. font data given to create() must
    // still be alive; only create()-made contexts can rescale.
    std::expected<void, font_error> set_scale(f32 scale);
    [[nodiscard]] u32 font_generation() const noexcept;
    // as a percentage (100 = 1:1). each change rebuilds the atlas (tens of ms per font): use steps of 5 / 10 or apply
    // a slider on release.
    [[nodiscard]] int scale_percent() const noexcept;
    std::expected<void, font_error> set_scale_percent(int percent) { return set_scale(static_cast<f32>(percent) / 100.0f); }
    // rebuilds the atlas at the current scale after release_font_pixels(), for a renderer recreated after device loss.
    // slow like set_scale; recreate the renderer from font(), then release_font_pixels() again
    std::expected<void, font_error> rebuild_font_atlas();

    // windows --------------------------------------------------------------
    [[nodiscard]] window_scope window(std::string_view title, vec2 initial_pos, f32 width = 280.0f)
    {
        return {*this, begin_window(title, initial_pos, width)};
    }
    bool begin_window(std::string_view title, vec2 initial_pos, f32 width = 280.0f);
    // size.y == 0: height follows content up to the display bottom, then scrolls. size is remembered when resizable.
    [[nodiscard]] window_scope window(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags)
    {
        return {*this, begin_window(title, initial_pos, size, flags)};
    }
    bool begin_window(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags);
    void end_window();

    // docking --------------------------------------------------------------
    // window_flags::dockable windows drag by the title bar into a dock space (centre = tab, edges = split). up to 8
    // spaces; one not called this frame is gone. while dragging: Shift = no docking, Esc cancels. tabs reorder by
    // dragging; double-click floats a tab or resets a splitter / edge dock.
    //   dock_area(rect), dock_area("name", rect)   the main space / a named one
    //   dock_edge(name, side, size, region)        side panel that takes room only while occupied; returns the rest:
    //       rect client = ui.dock_edge("explorer", dock_side::left, 260, {{0, bar_h}, ui.display_size()});
    //       ui.dock_area(ui.dock_edge("console", dock_side::bottom, 200, client));
    //   floating_dock(title, pos, size)            a movable dock space
    void dock_area(const rect& area);
    void dock_area(std::string_view space, const rect& area);
    [[nodiscard]] rect dock_edge(std::string_view space, dock_side side, f32 size, const rect& region);
    bool floating_dock(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags = window_flags::none);
    // docks `title` beside `target` (already docked) or into `space` (empty = main), e.g. for a default layout.
    // `size` = share of the target an edge split gives the new window.
    bool dock_window(std::string_view title, dock_zone zone, std::string_view target = {}, f32 size = 0.5f,
                     std::string_view space = {});
    void undock_window(std::string_view title);
    // the arrangement as text. load may run before windows exist; matched by name, unknown ones skipped, missing ones
    // float. false (no change) for invalid text or > 32 panes.
    [[nodiscard]] std::string dock_save_layout() const;
    bool dock_load_layout(std::string_view text);
    [[nodiscard]] bool is_docked(std::string_view title) const noexcept;

    // all user arrangement in one config section (config.hpp): dock layout, window places / sizes / collapse, table
    // columns, open tree nodes, scroll offsets. load any time; items are matched by id and applied when they appear.
    //   strata::config_file settings{"ui.ini"};
    //   settings.load();  ui.load_state(settings.data());
    //   ...  ui.save_state(settings.data());  settings.save();
    // save_state replaces the section. load_state is false (no change) for a section of another version.
    void save_state(config& cfg, std::string_view section = "ui") const;
    bool load_state(const config& cfg, std::string_view section = "ui");
    // window screen rect as of its last begin / end ({} if unknown)
    [[nodiscard]] rect window_rect(std::string_view title) const noexcept;

    // widgets --------------------------------------------------------------
    void text(std::string_view s);
    void text_dim(std::string_view s);
    void text_colored(color c, std::string_view s);
    // text() wrapped to the layout width (or set_next_item_width)
    void text_wrapped(std::string_view s);
    void text_wrapped_colored(color c, std::string_view s);

    // formats on the stack, falling back to a string when longer (never cuts utf-8)
    template <class... args>
    void textf(std::format_string<args...> fmt, args&&... a)
    {
        std::array<char, 512> buf;
        const auto r = std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()), fmt, std::forward<args>(a)...);
        if (static_cast<std::size_t>(r.size) <= buf.size()) {
            text({buf.data(), static_cast<std::size_t>(r.size)});
        } else {
            text(std::format(fmt, std::forward<args>(a)...)); // (formatting never moves from its arguments)
        }
    }

    bool button(std::string_view label);
    // with the identity kept apart from the caption
    bool button(std::string_view label, std::string_view id);
    bool checkbox(std::string_view label, bool& value);
    bool toggle(std::string_view label, bool& value);
    void progress_bar(f32 fraction, std::string_view overlay = {});
    void separator();
    void spacing(f32 height = 0.0f);
    void same_line() noexcept;
    // same line, `offset_x` from the content's left edge
    void same_line(f32 offset_x) noexcept;
    // same line, an item `width` wide ending at the content's right edge (after padding and scrollbar). inside a right
    // gutter it reaches the real edge.
    void same_line_right(f32 width) noexcept;
    // reserves `w` at the right until pop_right_gutter(): items lay out narrower, same_line_right() still reaches
    // the real edge.
    //   { auto g = ui.right_gutter(60.0f); ui.custom_item("header", {0, 24}); }   // 60 px for the row's buttons
    //   ui.same_line_right(24.0f); ui.icon_button(icons_font, icons::trash);
    void push_right_gutter(f32 w) noexcept;
    void pop_right_gutter() noexcept;
    [[nodiscard]] gutter_scope right_gutter(f32 w) noexcept { push_right_gutter(w); return gutter_scope{*this}; }
    // width of the next widget (slider, input, combo, progress, button)
    void set_next_item_width(f32 w) noexcept;

    // `icon` is drawn with `icon_font` (see icons.hpp), the label with the current font
    bool icon_button(font_id icon_font, std::string_view icon, std::string_view label = {});
    void icon_label(font_id icon_font, std::string_view icon, std::string_view label);

    // label top-left, value top-right, track with a round knob below
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

    // single-line field, true when changed. `hint` shows while empty.
    bool input_text(std::string_view label, std::string& value, std::string_view hint = {},
                    input_flags flags = input_flags::none, std::size_t max_bytes = 4096);
    // same into a secure_string (zeroed whenever freed or reallocated)
    bool input_text(std::string_view label, secure_string& value, std::string_view hint = {},
                    input_flags flags = input_flags::none, std::size_t max_bytes = 4096);
    // fixed buffer: null-terminated, `capacity` includes the terminator
    bool input_text(std::string_view label, char* buffer, std::size_t capacity, std::string_view hint = {},
                    input_flags flags = input_flags::none);

    // Enter = newline, Ctrl+Enter submits. size.x == 0: available width, size.y == 0: ~6 lines. true when changed.
    bool input_multiline(std::string_view label, std::string& value, vec2 size = {},
                         input_flags flags = input_flags::none, std::string_view hint = {},
                         std::size_t max_bytes = std::size_t{1} << 20);
    bool input_multiline(std::string_view label, secure_string& value, vec2 size = {},
                         input_flags flags = input_flags::none, std::string_view hint = {},
                         std::size_t max_bytes = std::size_t{1} << 20);

    // masked field; `value` always holds the formatted text. # digit, A letter, U / L letter to upper / lower,
    // X letter or digit, ? anything, \ escapes, anything else is literal. no undo history. true when changed.
    //   ui.input_masked("phone", phone, "(###) ###-####");     ui.input_masked("plate", plate, "UU-###");
    bool input_masked(std::string_view label, std::string& value, std::string_view mask, std::string_view hint = {},
                      input_flags flags = input_flags::none);

    // code editor: no wrapping, plus (code_flags) line numbers, current line, bracket matching, auto indent, block
    // (un)indent with Tab / Shift+Tab, find / replace (Ctrl+F / Ctrl+H). colours via input_spans().
    // size.y == 0: ~12 lines.
    bool input_code(std::string_view label, std::string& value, vec2 size = {}, code_flags flags = code_default,
                    std::string_view hint = {}, std::size_t max_bytes = std::size_t{1} << 22, int tab_size = 4);
    // scrolls to a line (1-based) and moves the caret there if focused; call in input_code's id scope
    void code_goto_line(std::string_view label, int line);
    // opens the find / replace bar focused on `text` (empty: last search); call in input_code's id scope
    void code_find(std::string_view label, std::string_view text = {}, bool with_replace = false);

    // styles the NEXT input_text / input_multiline. recompute on change (old ranges are clamped meanwhile).
    // ignored for passwords; first span wins on overlap; max 4096.
    void input_spans(std::span<const text_span> spans);

    // IME: composition shows at the caret until confirmed. while ime_wanted() the host enables its IME with the
    // candidate window at ime_position() (physical, caret bottom-left) and caret height ime_line_height()
    // (win32_platform::set_ime()).
    [[nodiscard]] bool             ime_wanted() const noexcept;
    [[nodiscard]] vec2             ime_position() const noexcept;
    [[nodiscard]] f32              ime_line_height() const noexcept;
    [[nodiscard]] bool             ime_composing() const noexcept;
    [[nodiscard]] std::string_view ime_composition() const noexcept;

    // true when `current` changed
    bool combo(std::string_view label, int& current, const std::string_view* items, std::size_t count);
    bool combo(std::string_view label, int& current, std::initializer_list<std::string_view> items)
    {
        return combo(label, current, items.begin(), items.size());
    }

    // dropdown with a filter box (case-insensitive substring). only visible rows are submitted, so thousands are fine.
    // Up / Down / Enter / Esc. true when `current` changed.
    bool combo_filtered(std::string_view label, int& current, const std::string_view* items, std::size_t count,
                        std::string_view hint = "type to filter");
    bool combo_filtered(std::string_view label, int& current, std::initializer_list<std::string_view> items,
                        std::string_view hint = "type to filter")
    {
        return combo_filtered(label, current, items.begin(), items.size(), hint);
    }

    // multi-select dropdown over `count` flags; stays open while clicking. true when a flag changed.
    bool combo_multi(std::string_view label, bool* selected, const std::string_view* items, std::size_t count,
                     std::string_view placeholder = "none");
    bool combo_multi(std::string_view label, bool* selected, std::initializer_list<std::string_view> items,
                     std::string_view placeholder = "none")
    {
        return combo_multi(label, selected, items.begin(), items.size(), placeholder);
    }

    // true when `selected` changed; the caller draws the content
    bool tab_bar(std::string_view id, const tab_desc* tabs, std::size_t count, int& selected, font_id icon_font = 0);
    bool tab_bar(std::string_view id, std::initializer_list<tab_desc> tabs, int& selected, font_id icon_font = 0)
    {
        return tab_bar(id, tabs.begin(), tabs.size(), selected, icon_font);
    }
    // closable / reorderable tabs and an add button. overflowing tabs scroll (wheel) and get a list menu.
    //   auto ev = ui.tab_bar("docs", tabs.data(), tabs.size(), current, tab_bar_flags::closable | tab_bar_flags::reorderable);
    //   apply_tab_events(ev, tab_list, current);
    tab_events tab_bar(std::string_view id, const tab_desc* tabs, std::size_t count, int& selected, tab_bar_flags flags,
                       font_id icon_font = 0);

    // color -------------------------------------------------------------
    // swatch + hex; click opens the full picker in a popup
    bool color_edit(std::string_view label, color& c, color_flags flags = color_flags::none);
    // inline picker: sv square, hue bar, alpha bar, preview + hex
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
    [[nodiscard]] bool item_pressed() const noexcept; // last tree row / selectable was clicked
    void text_ellipsis(std::string_view s); // text() cut with "..." to the available width

    // small controls in the right end of the row just submitted: placed right to left, clipped to the row, they take
    // the press from it. reserve room first with set_next_item_gutter() so the row's label is elided before them.
    //   ui.set_next_item_gutter(56.0f);
    //   const bool row = ui.selectable(name, id, selected);
    //   if (ui.row_accessory_button(icon_font, icons::trash)) { destroy(); }
    //   ui.row_accessory_checkbox("on", object.enabled);
    bool row_accessory_button(font_id icon_font, std::string_view icon, std::string_view id = {});
    bool row_accessory_checkbox(std::string_view id, bool& value);
    bool row_accessory_toggle(std::string_view id, bool& value);
    // room for accessories at the right of the next row only
    void set_next_item_gutter(f32 width) noexcept;

    // applies a click on row `index` (plain / Ctrl / Shift). true when the selection changed.
    bool selection_click(selection_state& sel, int index) const;

    // `id` is hashed after the label and never shown: repeated labels without "name##suffix" strings.
    // push_id(const void*) / push_id(u64) do the same for a subtree.
    //   ui.selectable(node.name, {reinterpret_cast<const char*>(&node), sizeof(void*)}, node.id == selected);
    bool tree_node(std::string_view label, std::string_view id, tree_flags flags);
    bool tree_leaf(std::string_view label, std::string_view id, bool selected);
    bool selectable(std::string_view label, std::string_view id, bool selected);
    [[nodiscard]] tree_scope tree(std::string_view label, std::string_view id, tree_flags flags)
    {
        return {*this, tree_node(label, id, flags)};
    }

    // forces the next tree_node / table_tree_node open or closed
    void set_next_item_open(bool open) noexcept;
    // ... recursively. Ctrl / Shift + arrow click does the same.
    void set_next_item_open_recursive(bool open) noexcept;
    // opens / closes every tree node in the current id scope (all nodes at top level); applies when next submitted.
    void open_all_tree_nodes() noexcept { tree_set_bulk(current_seed(), true); }
    void close_all_tree_nodes() noexcept { tree_set_bulk(current_seed(), false); }

    // scrolling ------------------------------------------------------------
    // innermost scrolling region (child, else window); logical pixels from content top, 0 if not scrolling
    [[nodiscard]] f32 scroll_y() const noexcept;
    [[nodiscard]] f32 scroll_max_y() const noexcept;
    void set_scroll_y(f32 y) noexcept;
    // horizontal offset of the innermost child_flags::horizontal region; 0 elsewhere
    [[nodiscard]] f32 scroll_x() const noexcept;
    [[nodiscard]] f32 scroll_max_x() const noexcept;
    void set_scroll_x(f32 x) noexcept;
    void scroll_to_top() noexcept { set_scroll_y(0.0f); }
    void scroll_to_bottom() noexcept { set_scroll_y(scroll_max_y()); }
    // scrolls minimally to bring the last item into view (applies next frame; call every frame to follow a selection)
    void ensure_item_visible() noexcept;
    // ... centred instead
    void scroll_to_item() noexcept;
    // reserves `height` without submitting (for self-culled rows); skip_items does the same for equal rows
    void skip_item(f32 height);
    void skip_items(int count, f32 item_height);

    // keyboard navigation ---------------------------------------------------
    // rows (selectable, tree_node, tree_leaf) between nav_begin() and nav_end() are navigable: Up / Down, Home / End,
    // PageUp / PageDown (10), Left / Right close / open or step out / in, Enter activates like a click. clicks move the
    // cursor. the cursor persists per scope and the view follows it. inactive while a text field has the keyboard.
    //   if (auto nav = ui.navigation("tree")) { ... rows ... }
    void nav_begin(std::string_view id);
    void nav_end();
    [[nodiscard]] nav_scope navigation(std::string_view id) { nav_begin(id); return nav_scope{*this}; }
    // the nav scope had the keyboard this frame
    [[nodiscard]] bool nav_active() const noexcept;
    // the last item has the keyboard (nav cursor or focused text field)
    [[nodiscard]] bool item_focused() const noexcept;

    // tables ---------------------------------------------------------------
    //   if (ui.begin_table("files", 2)) {
    //       ui.table_setup_column("Name");                              // stretch
    //       ui.table_setup_column("Size", 80);                          // fixed 80 px (3rd arg: stretch weight)
    //       int sort = ui.table_headers_row(sort_column, ascending);    // clicked column or -1
    //       for (auto& r : rows) {
    //           if (ui.table_next_row()) {                              // false when scrolled out: skip cells
    //               ui.table_next_column(); ui.text(r.name);
    //               ui.table_next_column(); ui.textf("{}", r.size);
    //           }
    //       }
    //       ui.end_table();
    //   }
    // height > 0: scrolling body under a fixed header. with hideable / reorderable columns, still fill cells in declared
    // order; table_next_column() is false for hidden ones and indices are always declared ones.
    bool begin_table(std::string_view id, u32 columns, table_flags flags = table_default, f32 height = 0.0f);
    void table_setup_column(std::string_view label, f32 fixed_width = 0.0f, f32 stretch_weight = 1.0f,
                            table_column_flags flags = table_column_flags::none);
    int  table_headers_row(int sort_column = -1, bool ascending = true);
    bool table_next_row();
    bool table_next_column();
    void end_table();
    // skips `count` rows (row height each, keeps the scrollbar right); for big tables with list_clipper
    void table_skip_rows(int count);
    // the body of a table with a height scrolls on its own, apart from scroll_y(). call between the rows and
    // end_table(): the offset, last frame's maximum, and a new offset (applied from the next frame, clamped)
    [[nodiscard]] f32 table_scroll_y() const noexcept;
    [[nodiscard]] f32 table_scroll_max_y() const noexcept;
    void table_set_scroll_y(f32 y) noexcept;
    // column order / widths / hidden state as text; call in begin_table's id scope, load may precede first show
    [[nodiscard]] std::string table_save_layout(std::string_view id) const;
    void table_load_layout(std::string_view id, std::string_view text);

    // tree tables: use these in a cell instead of text(); following rows are the children.
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
    // see list_clipper: these reserve the space above / below the visible rows (`item_height` 0 = one text line; in a
    // table, its row height)
    void list_clip_begin(int count, f32 item_height, int& first, int& last);
    void list_clip_end(int count, f32 item_height, int last);

    // rich text ------------------------------------------------------------
    // markup: <f=N>..</f> font id N, <c=rrggbb[aa]>..</c> colour, <a=href>..</a> link (underlined accent, hand cursor).
    // tags nest, "<<" is a literal '<'. strata never opens links; rich_link_clicked() reports the href.
    //   ui.rich_text("see <a=https://example.com>the manual</a> or <a=cmd:reset>reset</a>");
    //   if (auto href = ui.rich_link_clicked(); !href.empty()) { open(href); }
    // links in widget captions (rich_labels()) are not clickable.
    [[nodiscard]] std::string_view rich_link_clicked() const noexcept;
    // href under the pointer now
    [[nodiscard]] std::string_view rich_link_hovered() const noexcept;

    void rich_text(std::string_view markup);
    // wrapped to the layout width (or set_next_item_width)
    void rich_text_wrapped(std::string_view markup);

    // while alive, widget labels are parsed as markup; ids still hash the raw string.
    //   auto rich = ui.rich_labels();  ui.button("<f=1>save</f>");
    void push_rich_labels() noexcept;
    void pop_rich_labels() noexcept;
    [[nodiscard]] rich_scope rich_labels() noexcept { push_rich_labels(); return rich_scope{*this}; }
    [[nodiscard]] bool       rich_labels_active() const noexcept;

    template <class... args>
    void rich_textf(std::format_string<args...> fmt, args&&... a)
    {
        std::array<char, 512> buf; // (see textf)
        const auto r = std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()), fmt, std::forward<args>(a)...);
        if (static_cast<std::size_t>(r.size) <= buf.size()) {
            rich_text({buf.data(), static_cast<std::size_t>(r.size)});
        } else {
            rich_text(std::format(fmt, std::forward<args>(a)...));
        }
    }

    // containers -----------------------------------------------------------
    // clipped scrolling area. size.x == 0: rest of the line, size.y == 0: to the bottom of a fixed-height window.
    // pair with end_child(), or use child().
    bool begin_child(std::string_view id, vec2 size = {}, child_flags flags = child_flags::none);
    void end_child();
    [[nodiscard]] child_scope child(std::string_view id, vec2 size = {}, child_flags flags = child_flags::none)
    {
        return {*this, begin_child(id, size, flags)};
    }

    // titled box around related options, grows with content
    bool begin_card(std::string_view title, std::string_view icon = {}, font_id icon_font = 0);
    void end_card();
    [[nodiscard]] card_scope card(std::string_view title, std::string_view icon = {}, font_id icon_font = 0)
    {
        return {*this, begin_card(title, icon, icon_font)};
    }

    // vertical tab strip, true when changed. height 0 fills a fixed-height window; width 0 fits labels (or icons).
    bool tab_strip(std::string_view id, const tab_desc* tabs, std::size_t count, int& selected, font_id icon_font = 0,
                   f32 width = 0.0f, tab_strip_flags flags = tab_strip_flags::none, f32 height = 0.0f);
    bool tab_strip(std::string_view id, std::initializer_list<tab_desc> tabs, int& selected, font_id icon_font = 0,
                   f32 width = 0.0f, tab_strip_flags flags = tab_strip_flags::none, f32 height = 0.0f)
    {
        return tab_strip(id, tabs.begin(), tabs.size(), selected, icon_font, width, flags, height);
    }

    // images ---------------------------------------------------------------
    // size.x == 0: layout width, size.y == 0: square. [uv0, uv1] is the region shown, `tint` multiplies.
    void image(texture_id tex, vec2 size, vec2 uv0 = {0.0f, 0.0f}, vec2 uv1 = {1.0f, 1.0f},
               color tint = {255, 255, 255, 255}, f32 rounding = 0.0f);
    // true when clicked
    bool image_button(std::string_view label, texture_id tex, vec2 size, vec2 uv0 = {0.0f, 0.0f},
                      vec2 uv1 = {1.0f, 1.0f}, color tint = {255, 255, 255, 255});

    // transitions and tooltips -------------------------------------------------
    // multiplies the alpha of everything until pop_alpha()
    void push_alpha(f32 a) noexcept;
    void pop_alpha() noexcept;
    // fades + slides content when `page` changes; keep the scope alive while drawing the page
    [[nodiscard]] transition_scope page_transition(std::string_view key, int page, f32 slide = 14.0f);

    // after the last widget is hovered for style::tooltip_delay_s
    void tooltip(std::string_view text);
    [[nodiscard]] bool item_hovered() const noexcept;

    // the last hit-tested widget. right / middle clicks report on press, left on release over it (like the widget)
    [[nodiscard]] rect item_rect() const noexcept;
    [[nodiscard]] bool item_clicked(mouse_button b = mouse_button::left) const noexcept;
    // second click within the double-click time
    [[nodiscard]] bool item_double_clicked() const noexcept;

    // edit start / end of the value widget just submitted, for undo (its return value fires every frame of a drag).
    //   ui.slider("volume", volume, 0.0f, 1.0f);
    //   if (ui.item_activated()) { before = volume; }
    //   if (ui.item_deactivated_after_edit()) { undo.push(before, volume); }
    // instant edits (combo pick, keyboard toggle) activate and deactivate on the same frame.
    [[nodiscard]] bool item_active() const noexcept;
    [[nodiscard]] bool item_activated() const noexcept;
    [[nodiscard]] bool item_deactivated() const noexcept;
    [[nodiscard]] bool item_edited() const noexcept;
    [[nodiscard]] bool item_deactivated_after_edit() const noexcept;

    // lets later overlapping widgets take the press and hover from the last widget (default: first one wins).
    //   auto row = ui.custom_item("component", {0, 26});
    //   ui.allow_item_overlap();
    //   ui.same_line_right(24.0f);
    //   if (ui.icon_button(icons_font, icons::trash)) { remove(); }
    //   if (row.pressed && !ui.item_claimed()) { select(); }
    void allow_item_overlap() noexcept;
    // a later widget took the press from the allow_item_overlap() one
    [[nodiscard]] bool item_claimed() const noexcept;

    // click, then press a key or side mouse button. Esc cancels, Backspace / Delete clears. 0 = unbound.
    bool hotkey(std::string_view label, u32& key_code);
    // with modifiers (bare Esc cancels, bare Backspace / Delete unbinds)
    bool hotkey_chord(std::string_view label, key_chord& chord);
    // a chord sequence ("Ctrl+K, Ctrl+S"): each key commits at once, and another key within key_sequence_timeout
    // extends it (up to max_steps). bare Esc / Backspace / Delete as the first key cancels / unbinds
    bool hotkey_sequence(std::string_view label, key_sequence& seq);

    // nested id scope until pop_id(). pointer / integer forms identify rows by object without "label##suffix".
    void push_id(std::string_view s) noexcept;
    void push_id(const void* p) noexcept;
    void push_id(u64 value) noexcept;
    void push_id(int value) noexcept { push_id(static_cast<u64>(static_cast<i64>(value))); }
    void pop_id() noexcept;

    // popups ---------------------------------------------------------------
    // a widget panel under the last widget (or at `pos`), closed by Esc or an outside click. open and draw in the same
    // id scope. `width` 0 = opener's width (min 180).
    //   if (ui.button("options")) { ui.toggle_popup("opts"); }
    //   if (auto p = ui.popup("opts")) { ui.checkbox("wrap", wrap); if (ui.button("done")) { ui.close_popup(); } }
    // popups (and dropdowns / pickers, which are popups too) stack: one opened while a popup is being drawn goes on top
    // of it and the parent stays open, up to 4 levels. a click inside one closes the ones above it, a click outside
    // all of them closes them all, Esc closes the top one. opened from outside every popup, it replaces what was open.
    void open_popup(std::string_view id);
    void open_popup(std::string_view id, vec2 pos);
    void toggle_popup(std::string_view id);
    bool begin_popup(std::string_view id, f32 width = 0.0f);
    void end_popup();
    // closes the popup being drawn and those above it; outside every popup, the top one
    void close_popup() noexcept;
    void close_all_popups() noexcept;
    // open at any level (a parent stays open while its child is)
    [[nodiscard]] bool popup_is_open(std::string_view id) const noexcept;
    [[nodiscard]] popup_scope popup(std::string_view id, f32 width = 0.0f) { return {*this, begin_popup(id, width)}; }
    // screen rect of the last hit-tested widget
    [[nodiscard]] rect last_item_rect() const noexcept;

    // drag and drop ----------------------------------------------------------
    // the payload is a small typed copy of your data; targets accept one type name. Esc cancels.
    //   ui.selectable(row.name);
    //   if (auto d = ui.drag_source("row", i)) { ui.text(row.name); }         // preview under the pointer
    //   ...
    //   ui.selectable(other.name);
    //   if (auto drop = ui.drop_target("row")) { move_row(drop.as<int>(), j); }
    // begin_drag_source() / end_drag_source() are unscoped. the release ending a drag is not a click.
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
    [[nodiscard]] bool dragging() const noexcept;
    [[nodiscard]] std::string_view drag_payload_type() const noexcept;

    // date and time pickers ---------------------------------------------------
    // a field that opens a calendar or an hour / minute (/ second) grid. true when changed. see datetime.hpp
    bool date_picker(std::string_view label, date& value);
    bool time_picker(std::string_view label, time_of_day& value, bool seconds = false);
    bool datetime_picker(std::string_view label, date& d, time_of_day& t, bool seconds = false);

    // small status widgets ----------------------------------------------------
    // indeterminate spinner; `diameter` 0 fits the line
    void spinner(f32 diameter = 0.0f, color c = {0, 0, 0, 0});
    // non-interactive pill ("3", "new"); kind picks the colour
    void badge(std::string_view text, toast_kind kind = toast_kind::info);
    void badge(std::string_view text, color tint);
    // clickable tag with an optional x; flows with same_line()
    chip_result chip(std::string_view label, const chip_options& options = {});

    // drag sliders and number inputs ---------------------------------------
    // drag sideways (Shift fine, Alt coarse) or click to type. `speed` per pixel. lo < hi clamps and shows a ruler
    // (`speed` ignored).
    bool drag_float(std::string_view label, f32& value, f32 speed = 0.05f, f32 lo = 0.0f, f32 hi = 0.0f, int decimals = 2,
                    std::string_view suffix = {});
    bool drag_int(std::string_view label, int& value, f32 speed = 0.2f, int lo = 0, int hi = 0, std::string_view suffix = {});
    // 2 to 4 components under one caption
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
    // number text field; unparsable input leaves the value alone. - / + buttons when step > 0
    bool input_float(std::string_view label, f32& value, f32 step = 0.0f, int decimals = 3);
    bool input_int(std::string_view label, int& value, int step = 1);

    // plots ------------------------------------------------------------------
    // `values` may be a ring buffer (`offset` = oldest). lo / hi = plot_auto fits the data.
    void plot_lines(std::string_view label, std::span<const f32> values, vec2 size = {0.0f, 80.0f}, std::string_view overlay = {},
                    f32 lo = plot_auto, f32 hi = plot_auto, u32 offset = 0);
    void plot_histogram(std::string_view label, std::span<const f32> values, vec2 size = {0.0f, 80.0f},
                        std::string_view overlay = {}, f32 lo = plot_auto, f32 hi = plot_auto, u32 offset = 0);
    // tiny inline chart, no axes or caption
    void sparkline(std::span<const f32> values, vec2 size = {80.0f, 20.0f}, color c = {0, 0, 0, 0}, u32 offset = 0);
    // several series with a legend (length = shortest series)
    void plot(std::string_view label, std::span<const plot_series> series, vec2 size = {0.0f, 110.0f},
              plot_kind kind = plot_kind::lines, f32 lo = plot_auto, f32 hi = plot_auto, u32 offset = 0);
    // full chart with axes, optional zoom / pan. x follows the data, y the visible samples, unless fixed or zoomed.
    void plot(std::string_view label, std::span<const plot_series> series, const plot_options& options);
    void plot_reset_view(std::string_view label);
    // whether the chart is zoomed / panned, and last frame's x range ({0, 0} before the first)
    [[nodiscard]] bool plot_zoomed(std::string_view label) const;
    [[nodiscard]] vec2 plot_x_range(std::string_view label) const;

    // selectable text ----------------------------------------------------------
    // wrapped, mouse-selectable, copyable. the one-argument form keys on the text, so changing text needs an `id`.
    void text_selectable(std::string_view text) { text_selectable(text, text); }
    void text_selectable(std::string_view id, std::string_view text);
    // while alive, text() / text_dim() / textf() / text_wrapped() are selectable
    void push_selectable_text() noexcept;
    void pop_selectable_text() noexcept;
    [[nodiscard]] selectable_text_scope selectable_text() noexcept { push_selectable_text(); return selectable_text_scope{*this}; }

    // modal windows and dialogs ---------------------------------------------------
    // open_modal("Confirm") once, then every frame: if (auto m = ui.modal("Confirm")) { ... ui.close_modal(); }
    // centred, dims and blocks below; modals stack, the top one has input.
    void open_modal(std::string_view title);
    void close_modal();                 // closes the topmost
    [[nodiscard]] bool modal_open() const noexcept;
    [[nodiscard]] modal_scope modal(std::string_view title, vec2 size = {420.0f, 0.0f}, modal_flags flags = modal_flags::esc_closes)
    {
        return {*this, begin_modal(title, size, flags)};
    }
    bool begin_modal(std::string_view title, vec2 size = {420.0f, 0.0f}, modal_flags flags = modal_flags::esc_closes);
    void end_modal();
    // 0 while open / never opened, 1..n = button pressed, -1 = Esc / outside click. closes itself on a result.
    //   switch (ui.dialog("Delete?", "This cannot be undone.", {"Delete", "Cancel"})) { case 1: ...; }
    int dialog(std::string_view title, std::string_view message, std::initializer_list<std::string_view> buttons,
               modal_flags flags = modal_flags::esc_closes | modal_flags::backdrop_closes);

    // self-managing confirmation that carries its subject, so the caller keeps no state:
    //   if (ui.button("Delete")) { ui.ask_confirm("del", "Delete this object?", object_id); }
    //   switch (ui.confirm("del", {"Delete", "Cancel"}, {.remember = &never_ask})) {
    //       case 1: destroy(static_cast<u32>(ui.confirm_data())); break;
    //   }
    // returns 0 idle, 1..n button, -1 Esc / outside. with *remember true it never opens and returns `remembered` on
    // the ask_confirm() frame. `id` is global like a modal title.
    void ask_confirm(std::string_view id, std::string_view message, u64 user_data = 0);
    int  confirm(std::string_view id, std::initializer_list<std::string_view> buttons, const confirm_options& options = {});
    // ask_confirm()'s data, valid while open and on the answering frame
    [[nodiscard]] u64 confirm_data() const noexcept;

    // menus ------------------------------------------------------------------------------------
    // a bar across the top of the display:
    //   if (auto bar = ui.main_menu_bar()) { if (auto m = ui.menu("File")) { if (ui.menu_item("Open", "Ctrl+O")) ...; } }
    bool begin_main_menu_bar();
    void end_main_menu_bar();
    [[nodiscard]] menu_scope main_menu_bar() { return {*this, begin_main_menu_bar(), menu_scope::kind::main_bar}; }
    [[nodiscard]] f32 main_menu_bar_height() const noexcept;
    // menu in the bar, or a submenu
    bool begin_menu(std::string_view label, bool enabled = true);
    void end_menu();
    [[nodiscard]] menu_scope menu(std::string_view label, bool enabled = true)
    {
        return {*this, begin_menu(label, enabled), menu_scope::kind::submenu};
    }
    // true when clicked (closes all menus unless options.keep_open). '&' marks a mnemonic ("&&" = literal);
    // Alt + mnemonic opens a bar menu.
    bool menu_item(std::string_view label, std::string_view shortcut = {}, bool selected = false, bool enabled = true);
    bool menu_item(std::string_view label, bool& checked, std::string_view shortcut = {}, bool enabled = true);
    bool menu_item(std::string_view label, const menu_item_options& options);
    bool menu_item(std::string_view label, bool& checked, const menu_item_options& options);
    // true on the frame the combination ("Ctrl+O", "F5" ...) is pressed, modifiers exact. with a text field focused only
    // Ctrl / Alt combinations count, except the field's own (Ctrl+A/C/V/X/Z/Y)
    [[nodiscard]] bool accelerator(std::string_view combo) const;
    // same for a stored chord (see keybinds). never fires while a hotkey field is capturing
    [[nodiscard]] bool chord_pressed(const key_chord& chord) const;
    // true when the last step is pressed, each within key_sequence_timeout (Esc or a pause resets).
    // a one-step sequence behaves like chord_pressed
    [[nodiscard]] bool sequence_pressed(const key_sequence& seq) const;
    void menu_separator();
    // opens on right-click of the last widget or the given area:
    //   if (auto m = ui.context_menu("row")) { if (ui.menu_item("Delete")) ...; }
    bool begin_context_menu(std::string_view id);
    bool begin_context_menu(std::string_view id, const rect& area);
    // without the right click: open_popup_menu("id", pos), then begin_popup_menu("id") every frame
    void open_popup_menu(std::string_view id, vec2 pos);
    bool begin_popup_menu(std::string_view id);
    void end_popup_menu();
    [[nodiscard]] menu_scope context_menu(std::string_view id) { return {*this, begin_context_menu(id), menu_scope::kind::popup}; }
    [[nodiscard]] menu_scope context_menu(std::string_view id, const rect& area)
    {
        return {*this, begin_context_menu(id, area), menu_scope::kind::popup};
    }
    [[nodiscard]] menu_scope popup_menu(std::string_view id) { return {*this, begin_popup_menu(id), menu_scope::kind::popup}; }
    [[nodiscard]] bool menu_is_open() const noexcept;

    // toasts -----------------------------------------------------------------------------------
    // notifications stacked in a corner. hover pauses, click dismisses.
    toast_handle toast(std::string_view text, toast_kind kind = toast_kind::info, f32 seconds = 3.5f) { return toast({}, text, kind, seconds); }
    toast_handle toast(std::string_view title, std::string_view text, toast_kind kind = toast_kind::info, f32 seconds = 3.5f);
    // with buttons and / or progress (closed by its x only).
    //   toast_handle h = ui.toast({.title = "Deleted", .text = "3 files", .seconds = 8, .actions = acts});
    //   every frame:  if (ui.toast_action(h) == 0) undo();
    //   h = ui.toast({.title = "Downloading", .seconds = 0, .progress = 0});  ui.toast_progress(h, 0.4f, "40%");
    toast_handle toast(const toast_options& options);
    // index of the button pressed since the last call, once; -1 if none or closed
    [[nodiscard]] int toast_action(toast_handle h);
    // >= 1 completes (closes 2 s later); toast_busy = indeterminate
    void toast_progress(toast_handle h, f32 fraction, std::string_view text = {});
    void toast_close(toast_handle h);
    [[nodiscard]] bool toast_alive(toast_handle h) const noexcept;
    void clear_toasts() noexcept;
    void set_toast_corner(screen_corner corner) noexcept;
    [[nodiscard]] std::size_t toast_count() const noexcept;

    // log view ---------------------------------------------------------------------------------
    // toolbar (filter, level, follow, copy, clear) over `log`; only visible rows are drawn. size.y == 0 fills down.
    void log_view(std::string_view id, log_buffer& log, vec2 size = {0.0f, 0.0f}, log_view_flags flags = log_view_flags::none);

    // docking animation -----------------------------------------------------------------------
    // panes slide on dock / undock / close (default on)
    void set_dock_animation(bool on) noexcept;

    // smooth wheel scrolling over ~0.1 s (default on)
    void set_scroll_smoothing(bool on) noexcept;

    // true when pressed this frame with exactly these modifiers (auto-repeat included). a focused text field consumes
    // its keys when drawn, so ask before it (or use accelerator() / chord_pressed())
    [[nodiscard]] bool key_pressed(key k, bool ctrl = false, bool shift = false) const noexcept;
    // focuses the field with this label (current id scope) next time it is drawn, text selected. safe to call every
    // frame: an already focused field is left alone.
    void request_text_focus(std::string_view label) noexcept;
    // focused text field id (0 = none), and the same by label
    [[nodiscard]] id   focused_field() const noexcept;
    [[nodiscard]] bool field_focused(std::string_view label) const noexcept;

    // raw virtual-key codes ('A'..'Z', '0'..'9', VK_*). key_pressed = edge, needs only input_state::pressed_key;
    // key_down = level, needs input_state::keys_held. both are quiet while a text / hotkey field has the keyboard.
    [[nodiscard]] bool key_pressed(u32 virtual_key, bool ctrl = false, bool shift = false, bool alt = false) const noexcept;
    [[nodiscard]] bool key_down(u32 virtual_key) const noexcept;

    // 0 left, 1 right, 2 middle. raw buttons, not widget-bound (see item_clicked()); clicked / released are this
    // frame's edges
    [[nodiscard]] bool mouse_down(int button) const noexcept;
    [[nodiscard]] bool mouse_clicked(int button) const noexcept;
    [[nodiscard]] bool mouse_released(int button) const noexcept;

    // focused window. window_focused() asks about the window being submitted, for panel-scoped shortcuts:
    //   if (ui.window_focused() && ui.key_pressed(VK_DELETE)) { destroy(); }
    [[nodiscard]] bool window_focused() const noexcept;
    [[nodiscard]] bool is_window_focused(std::string_view title) const noexcept;

    // modifier keys (as of this frame)
    [[nodiscard]] bool ctrl_down() const noexcept;
    [[nodiscard]] bool shift_down() const noexcept;
    [[nodiscard]] bool alt_down() const noexcept;

    // custom drawing -------------------------------------------------------
    // the frame's draw list, screen space. lands between neighbouring widgets; outside a window, behind all windows.
    [[nodiscard]] draw_list& draw() noexcept;

    // reserves a layout slot; returns its rect and hover / held / pressed. draw into it with draw()
    [[nodiscard]] item_result custom_item(std::string_view label, vec2 size);
    // ... with the identity kept apart from the label
    [[nodiscard]] item_result custom_item(std::string_view label, std::string_view id, vec2 size);
    // draws `s` cut with "..." past `max_width`; returns the drawn width
    f32 label_clipped(vec2 pos, f32 max_width, color c, std::string_view s, font_id f);
    f32 label_clipped(vec2 pos, f32 max_width, color c, std::string_view s) { return label_clipped(pos, max_width, c, s, current_font()); }

    // the last frame's items submitted / culled, geometry, and begin / end_frame times
    [[nodiscard]] const frame_stats& stats() const noexcept;

    // idling ------------------------------------------------------------------
    // an untouched ui produces identical geometry every frame:
    //
    //   ui.end_frame();
    //   if (ui.can_idle()) { /* skip render + present, sleep until the next message */ }
    //   else { renderer.render(ui.render_data()); present(); }
    //
    // frame_unchanged() hashes the geometry against the last frame (~0.1 ms per MB); animations_settling() is true while
    // any animation still moves (its last frames can repeat geometry, so the hash alone would stop it a pixel short).
    // an overlay must still re-render into the game's frame; the renderers skip the buffer upload when unchanged.
    [[nodiscard]] bool frame_unchanged() const noexcept;
    [[nodiscard]] bool animations_settling() const noexcept;
    // both: nothing new to draw
    [[nodiscard]] bool can_idle() const noexcept;
    // forces the next frame to count as changed (texture replaced, theme edited, resize with host-owned back buffers)
    void invalidate() noexcept;

    // how long a sleeping host may wait for input before the next frame (caret blink, pending tooltip ...).
    // 0 = run now (changed or animating); no_deadline = wait for input.
    //
    //   ui.end_frame();
    //   if (!ui.frame_unchanged()) { renderer.render(ui.render_data()); present(); }
    //   const f64 wait = ui.next_wake_seconds();
    //   MsgWaitForMultipleObjects(0, nullptr, FALSE, wait == no_deadline ? INFINITE : DWORD(wait * 1000), QS_ALLINPUT);
    //
    // pass the real elapsed time, sleep included, as the next delta_time.
    [[nodiscard]] f64 next_wake_seconds() const noexcept;

    // widget ids --------------------------------------------------------------
    // widgets hashing to one id share animation, press and focus state. debug builds count collisions
    // (stats().id_collisions) and name the first -- usually a repeated label: use "label##suffix" or push_id().
    // release builds return 0 / empty.
    [[nodiscard]] id   id_collision() const noexcept;
    [[nodiscard]] std::string_view id_collision_label() const noexcept;

    // draws everything stats() reports, overflows and collisions in red. call every frame; draws nothing while `open`
    // is false. an ordinary window with ids under "##strata_metrics".
    //   ui.debug_metrics_window(show_metrics);
    void debug_metrics_window(bool& open);
    // the live draw commands (clip, index count, texture, blur), one row each: shows what failed to merge
    void debug_draw_list_window(bool& open);

    // per-key value easing toward `target` at the theme's speed (or `speed`, 1/s); starts at `target`
    [[nodiscard]] f32 animate(std::string_view key, f32 target, f32 speed = 0.0f);

    [[nodiscard]] f32  content_width() const noexcept;
    // scrollbar width (already subtracted from content_width() when shown), and the visible rect of the innermost
    // scrolling region in logical screen coordinates
    [[nodiscard]] static constexpr f32 scrollbar_width() noexcept { return scrollbar_w; }
    [[nodiscard]] rect content_rect() const noexcept;
    // the press that activated the last tree row hit its arrow (what tree_flags::arrow_only tests)
    [[nodiscard]] bool item_arrow_hit() const noexcept;
    // the last row / text_ellipsis label was cut with "..." (add a tooltip)
    [[nodiscard]] bool item_truncated() const noexcept;
    [[nodiscard]] f32  frame_height() const noexcept;
    [[nodiscard]] vec2 mouse_pos() const noexcept;
    [[nodiscard]] vec2 display_size() const noexcept;

    // fonts ----------------------------------------------------------------
    // ids: context_config::font, then extra_fonts. applies to text and widget sizes until popped; titles use font 0.
    void push_font(font_id f) noexcept;
    void pop_font() noexcept;
    [[nodiscard]] font_scope with_font(font_id f) noexcept { push_font(f); return font_scope{*this}; }
    [[nodiscard]] font_id    current_font() const noexcept;

    // disabled items -------------------------------------------------------
    // until end_disabled(): faded and inert, but item_hovered() still works (for a tooltip). nests; an inner enabled
    // scope stays disabled.
    //   { auto d = ui.disabled_if(selection.empty()); if (ui.button("Delete")) { ... } ui.tooltip("select something first"); }
    void begin_disabled(bool disabled = true) noexcept;
    void end_disabled() noexcept;
    [[nodiscard]] disabled_scope disabled_if(bool disabled = true) noexcept { begin_disabled(disabled); return disabled_scope{*this}; }
    [[nodiscard]] bool item_enabled() const noexcept;

    // clipboard ------------------------------------------------------------
    // the text fields' clipboard hooks, for app copy / paste
    bool copy_text(std::string_view text) const;
    bool paste_text(std::string& out) const;

    // style overrides ------------------------------------------------------
    // apply until popped; unbalanced pushes are undone at the next begin_frame
    void push_color(style_color which, color c) noexcept;
    void pop_color(u32 count = 1) noexcept;
    void push_var(style_var which, f32 value) noexcept;
    void pop_var(u32 count = 1) noexcept;

    // several at once, popped at scope end
    //   auto danger = ui.style_overrides({override_color(style_color::accent, red), override_var(style_var::rounding, 14)});
    [[nodiscard]] style_scope style_overrides(std::initializer_list<style_override> list) noexcept;

    // access ---------------------------------------------------------------
    [[nodiscard]] strata::style&       theme() noexcept;
    [[nodiscard]] const strata::style& theme() const noexcept;
    [[nodiscard]] const font_atlas&    font() const noexcept;
    [[nodiscard]] u64                  frame_index() const noexcept;
    // ui time: sum of frame deltas
    [[nodiscard]] f64                  time() const noexcept;

    // frees the cpu copy of the font bitmap; call once the renderer exists
    void release_font_pixels() noexcept;

private:
    struct interaction;

    using anim_slot = internal::anim_slot; // src/core/animation.hpp

    using window_state = internal::window_state; // src/core/window_registry.hpp

    using layout_state = internal::layout_state; // src/core/layout_state.hpp

    using popup_stack = internal::popup_stack; // src/popup_state.hpp

    // a run of draw commands and its owner: windows restack / popups lift by reordering runs
    struct cmd_run;

    struct field_layout;

    static constexpr u32 max_tree_depth    = 16;
    static constexpr u32 max_table_columns = 16;
    static constexpr u32 max_tables        = 32;
    static constexpr u32 max_table_depth   = 4;

    struct tree_frame;
    struct tree_state;

    struct table_state;
    struct table_column;
    struct table_frame;

    struct child_state;
    struct child_frame;
    struct card_state;
    struct card_frame;

    // docking state lives in internal::dock_state (src/dock_state.hpp)
    static constexpr u8 no_node = 0xff;

    // undo history of the focused field
    enum class edit_kind : u8;
    struct edit_op;
    struct edit_history;
    struct ml_line;

    // rich text layout scratch
    struct rich_run;
    struct rich_seg;
    struct rich_line;

    // open popup chain (0 = root context / bar menu, 1.. = submenus)
    static constexpr u32 max_menu_levels = 4;
    using menu_level = internal::menu_level; // src/menu_state.hpp
    using menu_frame = internal::menu_frame; // src/menu_state.hpp
    using toast_entry = internal::toast_entry; // src/toast_state.hpp -- (toast, button) pressed and not yet read

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
    static constexpr u32 run_backdrop  = 0xfffffff0u; // + modal level - 1: the dim behind a modal

    [[nodiscard]] id       current_seed() const noexcept;
    // widget id from a label in the current scope; debug builds also record the label for collision reports
    [[nodiscard]] id       widget_id(std::string_view label) noexcept;
    // debug only: records `key` for this frame, counting repeats
    void                   check_id(id key) noexcept;
    [[nodiscard]] anim_slot& anim_for(id key) noexcept;            // finds or creates (and keeps alive)
    [[nodiscard]] anim_slot* anim_find(id key) noexcept;           // finds only
    struct press_anim;
    // idle buttons own no animation state
    [[nodiscard]] press_anim button_anim(id key, const interaction& in) noexcept;
    [[nodiscard]] f32      approach(f32 current, f32 target, f32 speed = 0.0f) const noexcept;
    [[nodiscard]] window_state* window_for(id key, vec2 pos, f32 width) noexcept;
    // report edit state: `session` id, `changed` return value, `engaged` held / typed / popup open now.
    // feeds item_activated() ... item_deactivated_after_edit()
    void track_edit(id session, bool changed, bool engaged) noexcept;
    // a composite reports for its parts: they stay quiet while this lives
    struct edit_mute;
    std::expected<void, font_error> build_font_atlas(f32 scale);
    void                   forget_window(id key) noexcept;
    // a fixed-size table / stack ran out: counted in frame_stats, reported once per `what`
    void                   report_limit(const char* what, u32 capacity) noexcept;
    // sends `message` to the diagnostics hook (or stderr) unless `once_key` was seen
    void                   diagnose(diagnostic_kind kind, std::string_view message, u64 once_key) noexcept;
    [[nodiscard]] rect     layout_place(vec2 size) noexcept;
    // rows outside the clip rect do no work (no hit test, animation, measuring or geometry); layout already advanced,
    // so scrolling and tree state are unaffected
    [[nodiscard]] bool     item_culled(const rect& r) noexcept;
    // bookkeeping for a culled row: it becomes the inert "last item"
    void                   note_culled_item(id key, const rect& r) noexcept;
    // non-interactive content: becomes the "last item", no press
    void                   note_passive_item(id key, const rect& r) noexcept;
    [[nodiscard]] vec2     measure_cached(font_id f, std::string_view s) noexcept;
    // nav: every row of an open nav scope registers here, culled or not
    void                   nav_record(id key, const rect& r, u32 depth, bool node, bool open) noexcept;
    [[nodiscard]] bool     nav_take(id key) noexcept;   // Enter was pressed with the cursor on this row
    [[nodiscard]] bool     nav_is_cursor(id key) const noexcept;
    void                   nav_click(id key) noexcept;  // a click moves the cursor
    // accessory strip of the row just submitted; false if there is no row
    [[nodiscard]] bool     accessory_slot(f32 width, rect& out) noexcept;
    // a row accessories can attach to (see row_anchor_)
    void                   note_row_anchor(id key, const rect& r) noexcept;
    void                   allow_item_overlap_at(id key, const rect& r) noexcept;
    // scroll offset / view of the innermost scrolling region, or nullptr
    [[nodiscard]] f32*     scroll_slot(rect& view) noexcept;
    // this frame's wheel scroll for a region: `unit` = one line / row, `page` = visible extent (0 if none), times
    // style::scroll_speed
    [[nodiscard]] f32      scroll_step(f32 unit, f32 page) const noexcept;
    [[nodiscard]] f32      wheel_scroll(f32 unit, f32 page = 0.0f) const noexcept;
    [[nodiscard]] f32      wheel_scroll_x(f32 unit, f32 page = 0.0f) const noexcept;
    void                   scroll_reveal_rect(const rect& item, bool center) noexcept;
    // tree open state, with set_next_item_open / bulk open-close applied
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
    // popup stack: opens `key` above the popup being drawn (from outside every popup: replaces the stack). false when
    // the stack is full
    bool popup_push(id key) noexcept;
    // enters the draw layer of `level` (records its rect as drawn this frame); popup_leave() restores what was before
    void popup_enter(u32 level, const rect& r, const rect& anchor) noexcept;
    void popup_leave() noexcept;
    bool picker_body(id key, color& c, color_flags flags);
    bool hotkey_field(std::string_view label, u32& key_code, key_chord* chord);
    // scrollbar thumb drag: the new offset while pressed. `grab` = press point in the thumb (off-thumb presses centre
    // it), `travel` = thumb range
    [[nodiscard]] f32 thumb_drag(const interaction& in, f32& grab, f32 thumb_y, f32 thumb_h, f32 track_top, f32 travel,
                                 f32 max_scroll, f32 scroll) const noexcept;
    [[nodiscard]] f32 thumb_drag_x(const interaction& in, f32& grab, f32 thumb_x, f32 thumb_w, f32 track_left,
                                   f32 travel, f32 max_scroll, f32 scroll) const noexcept;
    [[nodiscard]] f32 thumb_drag_along(f32 along, const interaction& in, f32& grab, f32 thumb_lo, f32 thumb_len,
                                       f32 track_lo, f32 travel, f32 max_scroll, f32 scroll) const noexcept;
    // innermost open child_flags::horizontal region (owns the x offset)
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
    // all edits of the focused field go through edit_replace (records undo)
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
    // popup background: the opaque shape, or acrylic when style_.popup_acrylic > 0
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

    // what set_scale needs to rebuild the atlas (owned strings; font_config views re-pointed)
    struct font_source;
    // zoomable chart views (survive between frames)
    struct chart_view;
    [[nodiscard]] chart_view& chart_view_for(id key) noexcept;
    void chart_impl(std::string_view label, std::span<const plot_series> series, const plot_options& o);

    // modals
    static constexpr u32 max_modals = 4;
    [[nodiscard]] u32 register_click() noexcept;      // 1 / 2 / 3 = single / double / triple click
    // caret blink phase (always on when blinking is disabled)
    [[nodiscard]] bool caret_visible() const noexcept;
    void ed_prepare_spans(std::size_t text_size, font_id base, f32& line_h, f32& ascent, bool ignore);
    [[nodiscard]] f32 ed_measure(font_id base, std::string_view t, std::size_t a, std::size_t b) const;
    [[nodiscard]] font_id ed_font_at(font_id base, std::size_t i) const noexcept;
    void ed_draw(vec2 pos, f32 line_ascent, color col, std::string_view t, std::size_t a, std::size_t b, font_id base);
    void draw_ime_chip(vec2 caret_bottom, f32 line_h, font_id f);
    void apply_pending_scroll(id key, f32& scroll_y, f32* scroll_x) noexcept;
    [[nodiscard]] static std::string table_layout_text(const table_state& t);   // field to focus when next drawn
    using code_mode = internal::code_mode; // src/code_state.hpp
    using code_state = internal::code_state; // src/code_state.hpp
    [[nodiscard]] code_state& code_state_for(id key);

    // label_size() cache: direct-mapped (font, string) -> size, cleared when the atlas changes. rich labels skip it.
    struct measure_slot;
    static constexpr u32 measure_cache_size = 4096;   // bulk open also applies to nodes revealed later

    // right gutter stack (push_right_gutter)
    static constexpr u32 max_gutter_depth = 8;

    // disabled scopes: depth, whether the outermost pushed the fade, and which scopes counted (keeps end_disabled
    // balanced)
    static constexpr u32 max_disabled_depth = 16;

    static constexpr f32 scrollbar_w = 10.0f;

    // edit sessions (track_edit): last widget's flags, the open session and whether it changed, engaged widgets this /
    // last frame
    struct edit_flags;     // the item (last_item_key_) the flags belong to
    struct edit_session;

    // private state (src/context_impl.hpp), so changes to it do not touch this header
    struct impl;
    std::unique_ptr<impl> m_;
};


// submits only the visible rows of a long list; the rest is reserved space so layout and scrollbar match the whole
// list. use inside a scrolling area; all rows must have the same height.
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

    // true once (submit begin() .. end()), then false
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
