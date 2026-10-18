// libFuzzer over every strata parser of foreign text, text editing, recorded input, the node editor and the C interface.
// the first byte picks the target.
// built by the x64-asan preset (STRATA_BUILD_FUZZERS, AddressSanitizer):
//   strata_fuzz corpus_dir -max_len=4096            (fuzz until stopped; crashes leave crash-<hash>)
//   strata_fuzz crash-<hash>                        (replay one input)
// failures: a crash, an asan report, or a violated property (checks abort).

#include <strata/strata.hpp>
#include <strata/strata_c.h>

#include "text_util.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

using namespace strata;

namespace {

// a property of the code under test that must hold for every input
#define FUZZ_CHECK(cond)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "fuzz check failed: %s (line %d)\n", #cond, __LINE__);    \
            std::abort();                                                                  \
        }                                                                                  \
    } while (false)

struct clip_store {
    std::string data;
};
void clip_set(void* user, std::string_view text) noexcept { static_cast<clip_store*>(user)->data.assign(text); }
bool clip_get(void* user, std::string& out) noexcept
{
    out = static_cast<clip_store*>(user)->data;
    return true;
}

// one context for the whole run: building the font atlas is what takes the time
context& shared_ui()
{
    static std::unique_ptr<context> ui;
    static clip_store               clip;
    if (ui == nullptr) {
        ui = std::make_unique<context>(context::create().value());
        ui->set_clipboard({&clip_set, &clip_get, &clip});
        ui->set_diagnostics({[](void*, const diagnostic&) noexcept {}, nullptr}); // (limits are expected here: quiet)
    }
    return *ui;
}

input_state quiet_input()
{
    input_state in;
    in.display_size = {800.0f, 600.0f};
    in.mouse_pos    = {-100.0f, -100.0f};
    return in;
}

void fuzz_config(std::string_view text)
{
    config c;
    (void)c.from_string(text);
    // what it wrote reads back as the same data
    const std::string once = c.to_string();
    config again;
    FUZZ_CHECK(again.from_string(once) == 0);
    FUZZ_CHECK(again.to_string() == once);
}

void fuzz_theme(std::string_view text)
{
    style s;
    (void)themes::from_string(text, s);
    const std::string once = themes::to_string(s);
    style back;
    FUZZ_CHECK(themes::from_string(once, back).ok());
    FUZZ_CHECK(themes::to_string(back) == once);
}

void fuzz_dock_layout(std::string_view text)
{
    context& ui = shared_ui();
    (void)ui.dock_load_layout(text);
    // the ui still runs on whatever was loaded
    for (int i = 0; i < 2; ++i) {
        ui.begin_frame(quiet_input());
        ui.dock_area({{0, 0}, {800, 600}});
        if (auto w = ui.window("a", {10, 10}, {200, 150}, window_flags::dockable)) { ui.text("a"); }
        if (auto w = ui.window("b", {50, 50}, {200, 150}, window_flags::dockable)) { ui.text("b"); }
        ui.end_frame();
    }
    const std::string saved = ui.dock_save_layout();
    FUZZ_CHECK(ui.dock_load_layout(saved)); // what it saves, it loads
}

void fuzz_state(std::string_view text)
{
    context& ui = shared_ui();
    config c;
    (void)c.from_string(text);
    c.set("ui", "version", "1"); // (past the version check, into the parsing)
    (void)ui.load_state(c);
    ui.begin_frame(quiet_input());
    if (auto w = ui.window("s", {10, 10}, {200, 150}, window_flags::none)) {
        if (ui.tree_node("n")) { ui.text("x"); ui.tree_pop(); }
    }
    ui.end_frame();
    config out;
    ui.save_state(out);
}

void fuzz_rich_text(std::string_view text)
{
    context& ui = shared_ui();
    ui.begin_frame(quiet_input());
    if (auto w = ui.window("r", {0, 0}, {400, 400}, window_flags::none)) {
        ui.rich_text(text);
        ui.rich_text_wrapped(text);
        ui.text(text);           // (bidi reordering, font lookup, kerning on arbitrary utf-8)
        ui.text_wrapped(text);
    }
    ui.end_frame();
}

void fuzz_chords(std::string_view text)
{
    key_chord c;
    if (chord_from_string(text, c)) {
        key_chord back;
        FUZZ_CHECK(chord_from_string(chord_to_string(c), back));
        FUZZ_CHECK(back == c);
    }
    key_sequence s;
    if (sequence_from_string(text, s)) {
        key_sequence back;
        FUZZ_CHECK(sequence_from_string(sequence_to_string(s), back));
        FUZZ_CHECK(back == s);
    }
}

// bytes drive an editing session: typing, keys with modifiers, clipboard shortcuts, clicks
void fuzz_editing(std::string_view ops)
{
    context& ui = shared_ui();
    static std::string single, multi;
    single.clear();
    multi.clear();
    bool  first = true;
    vec2  mouse{-100.0f, -100.0f};
    bool  down  = false;
    std::size_t i = 0;
    for (int frame = 0; frame < 64 && i < ops.size(); ++frame) {
        input_state in = quiet_input();
        // up to 8 operations per frame
        for (int n = 0; n < 8 && i < ops.size(); ++n) {
            const auto op = static_cast<u8>(ops[i++]);
            const auto arg = i < ops.size() ? static_cast<u8>(ops[i]) : u8{0};
            switch (op % 6) {
            case 0: // type a byte (utf-8 fragments included: the field has to cope)
                if (in.typed_len < in.typed.size()) { in.typed[in.typed_len++] = static_cast<char>(arg); }
                ++i;
                break;
            case 1: // a key with modifiers
                if (in.key_count < in.keys.size()) {
                    static constexpr key editing[] = {key::left, key::right, key::up, key::down, key::home, key::end, key::backspace,
                                                      key::del, key::enter, key::escape, key::tab, key::page_up, key::page_down,
                                                      key::a, key::c, key::v, key::x, key::z, key::y};
                    in.keys[in.key_count++] = {editing[arg % std::size(editing)], (arg & 0x40) != 0, (arg & 0x80) != 0, (arg & 0x20) != 0};
                }
                ++i;
                break;
            case 2: // the pointer moves
                mouse = {static_cast<f32>(arg) * 3.0f, static_cast<f32>(op) * 2.0f};
                ++i;
                break;
            case 3: // press / release
                down = !down;
                break;
            case 4: // a shortcut press (hotkeys, accelerators)
                in.press(static_cast<key>(arg), (arg & 1) != 0, (arg & 2) != 0, (arg & 4) != 0);
                ++i;
                break;
            default: // end the frame here
                n = 8;
                break;
            }
        }
        in.mouse_pos     = mouse;
        in.mouse_down[0] = down;
        ui.begin_frame(in);
        if (auto w = ui.window("e", {0, 0}, {500, 500}, window_flags::none)) {
            if (first) { ui.request_text_focus("multi"); }
            (void)ui.input_text("single", single, {}, input_flags::none, 256);
            (void)ui.input_multiline("multi", multi, {400.0f, 200.0f}, input_flags::none, {}, 4096);
            f32 num = 1.0f;
            (void)ui.drag_float("num", num);
        }
        ui.end_frame();
        first = false;
    }
    FUZZ_CHECK(single.size() <= 256);
    FUZZ_CHECK(multi.size() <= 4096);
}


// a recorded input is foreign data: loading either refuses it or yields frames the ui can run, and what loaded
// records back to something that loads again
void fuzz_replay(std::string_view bytes)
{
    input_replayer play;
    if (!play.load({reinterpret_cast<const u8*>(bytes.data()), bytes.size()})) { return; }
    context&      ui = shared_ui();
    input_recorder again;
    input_state   in;
    for (int i = 0; i < 6 && play.next(in); ++i) {
        again.record(in);
        // non-finite pointer positions are the platform's to filter, not part of what is checked here
        if (!std::isfinite(in.mouse_pos.x) || !std::isfinite(in.mouse_pos.y) || !std::isfinite(in.display_size.x) ||
            !std::isfinite(in.display_size.y) || !std::isfinite(in.wheel) || !std::isfinite(in.delta_time)) {
            continue;
        }
        in.display_size = {800.0f, 600.0f};
        ui.begin_frame(in);
        if (auto w = ui.window("replay", {10, 10}, {300, 200}, window_flags::none)) {
            std::string text = "text";
            (void)ui.input_text("field", text);
            (void)ui.button("button");
        }
        ui.end_frame();
    }
    input_replayer back;
    FUZZ_CHECK(back.load(again.data()));
    FUZZ_CHECK(back.frames() == again.frames());
}

// double-click word rules and word / line drag growth on any bytes
void fuzz_word_selection(std::string_view bytes)
{
    if (bytes.size() < 3) { return; }
    const std::size_t idx_a = static_cast<std::uint8_t>(bytes[0]) * 7u;
    const std::size_t idx_b = static_cast<std::uint8_t>(bytes[1]) * 5u;
    const std::string_view s = bytes.substr(2);
    std::size_t first = 0, last = 0;
    text::word_bounds(s, idx_a, first, last);
    FUZZ_CHECK(first <= last && last <= s.size());
    std::size_t anchor = 0, cursor = 0;
    for (const u32 clicks : {2u, 3u}) {
        text::extend_unit_selection(s, clicks, first, last, idx_b, anchor, cursor);
        FUZZ_CHECK(anchor <= s.size() && cursor <= s.size());
    }
}

// a node editor driven by scripted input over a graph made from the bytes
void fuzz_node_editor(std::string_view bytes)
{
    static node_editor ed;
    context& ui = shared_ui();
    std::array<vec2, 4> pos{};
    for (std::size_t i = 0; i < pos.size(); ++i) {
        pos[i] = {bytes.size() > 2 * i ? static_cast<float>(static_cast<std::uint8_t>(bytes[2 * i])) * 2.0f : 40.0f,
                  bytes.size() > 2 * i + 1 ? static_cast<float>(static_cast<std::uint8_t>(bytes[2 * i + 1])) * 2.0f : 40.0f};
    }
    const std::string_view script = bytes.size() > 8 ? bytes.substr(8) : std::string_view{};
    input_state in = quiet_input();
    for (std::size_t step = 0; step < std::min<std::size_t>(script.size(), 24) + 2; ++step) {
        const std::uint8_t op = step < script.size() ? static_cast<std::uint8_t>(script[step]) : 0;
        switch (op % 6) {
        case 0: in.mouse_pos = {static_cast<float>(op) * 3.0f, static_cast<float>(op / 2) * 4.0f}; break;
        case 1: in.mouse_down[0] = !in.mouse_down[0]; break;
        case 2: in.mouse_down[1] = !in.mouse_down[1]; break;
        case 3: in.wheel = static_cast<float>(static_cast<int>(op % 9) - 4); break;
        case 4: in.press(op % 2 == 0 ? key::del : static_cast<key>(0x41 + op % 26), op % 3 == 0); break;
        default: in.mouse_down[2] = !in.mouse_down[2]; break;
        }
        ui.begin_frame(in);
        if (auto w = ui.window("g", {0, 0}, {700, 500}, window_flags::none)) {
            if (ed.begin(ui, "canvas", {0.0f, 400.0f})) {
                for (std::size_t n = 0; n < pos.size(); ++n) {
                    ed.begin_node(n + 1, "node", pos[n]);
                    ed.input_pin((n + 1) * 10, "in", static_cast<u32>(n % 2));
                    ed.output_pin((n + 1) * 10 + 1, "out", static_cast<u32>(n % 2));
                    ed.end_node();
                }
                ed.link(11, 20);
                ed.link(31, 999); // a link to a pin that does not exist
                ed.end();
            }
            node_link l;
            (void)ed.link_created(l);
            (void)ed.link_deleted(l);
        }
        ui.end_frame();
        in.wheel     = 0.0f;
        in.key_count = 0;
        FUZZ_CHECK(ed.zoom >= ed.min_zoom && ed.zoom <= ed.max_zoom);
        FUZZ_CHECK(std::isfinite(ed.pan.x) && std::isfinite(ed.pan.y));
        for (const vec2& p : pos) { FUZZ_CHECK(std::isfinite(p.x) && std::isfinite(p.y)); }
        for (const u64 id : ed.selected_nodes()) { FUZZ_CHECK(id >= 1 && id <= pos.size()); }
    }
}

// the C interface under scripted input
void fuzz_c_api(std::string_view bytes)
{
    static strata_context* ui = strata_context_create(nullptr);
    FUZZ_CHECK(ui != nullptr);
    static char  buffer[32] = "x";
    static int   flag = 0, pick = 0;
    static float level = 0.5f;
    strata_input in{};
    in.display_w  = 800.0f;
    in.display_h  = 600.0f;
    in.delta_time = 1.0f / 60.0f;
    in.mouse_pos  = {-100.0f, -100.0f};
    for (std::size_t step = 0; step < std::min<std::size_t>(bytes.size(), 20) + 1; ++step) {
        const std::uint8_t op = step < bytes.size() ? static_cast<std::uint8_t>(bytes[step]) : 0;
        char typed[2] = {static_cast<char>(0x20 + op % 90), 0};
        in.typed_utf8 = op % 5 == 0 ? typed : nullptr;
        in.typed_len  = in.typed_utf8 != nullptr ? 1 : 0;
        in.key_count  = op % 7 == 0 ? 1 : 0;
        in.keys[0]    = op % 2 == 0 ? 0x08u : 0x25u;
        switch (op % 4) {
        case 0: in.mouse_pos = {static_cast<float>(op) * 3.0f, static_cast<float>(op) * 2.0f}; break;
        case 1: in.mouse_down[0] = in.mouse_down[0] != 0 ? 0 : 1; break;
        case 2: in.wheel = static_cast<float>(op % 5) - 2.0f; break;
        default: break;
        }
        strata_begin_frame(ui, &in);
        if (strata_begin_window(ui, "w", 10, 10, 320, 260, 0)) {
            strata_text(ui, "text");
            (void)strata_button(ui, "button");
            (void)strata_checkbox(ui, "flag", &flag);
            (void)strata_slider_float(ui, "level", &level, 0.0f, 1.0f);
            (void)strata_input_text(ui, "field", buffer, sizeof buffer);
            static const char* const items[] = {"a", "b"};
            (void)strata_combo(ui, "pick", &pick, items, 2);
            if (strata_begin_table(ui, "t", 2, 3, 0.0f)) {
                strata_table_setup_column(ui, "a", 80.0f);
                strata_table_setup_column(ui, "b", 0.0f);
                strata_table_headers_row(ui);
                (void)strata_table_next_row(ui);
                (void)strata_table_next_column(ui);
                strata_text(ui, "cell");
                strata_end_table(ui);
            }
        }
        strata_end_window(ui);
        strata_end_frame(ui);
        in.wheel = 0.0f;
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0) {
        return 0;
    }
    const std::string_view rest{reinterpret_cast<const char*>(data + 1), size - 1};
    switch (data[0] % 11) {
    case 0: fuzz_config(rest); break;
    case 1: fuzz_theme(rest); break;
    case 2: fuzz_dock_layout(rest); break;
    case 3: fuzz_state(rest); break;
    case 4: fuzz_rich_text(rest); break;
    case 5: fuzz_chords(rest); break;
    case 6: fuzz_editing(rest); break;
    case 7: fuzz_replay(rest); break;
    case 8: fuzz_word_selection(rest); break;
    case 9: fuzz_node_editor(rest); break;
    default: fuzz_c_api(rest); break;
    }
    return 0;
}
