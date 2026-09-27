// libFuzzer over every strata parser of foreign text, and over text editing. the first byte picks the target.
// built by the x64-asan preset (STRATA_BUILD_FUZZERS, AddressSanitizer):
//   strata_fuzz corpus_dir -max_len=4096            (fuzz until stopped; crashes leave crash-<hash>)
//   strata_fuzz crash-<hash>                        (replay one input)
// failures: a crash, an asan report, or a violated property (checks abort).

#include <strata/strata.hpp>

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
                    in.keys[in.key_count++] = {static_cast<key>(arg % 19), (arg & 0x40) != 0, (arg & 0x80) != 0, (arg & 0x20) != 0};
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
                if (in.press_count < in.presses.size()) { in.presses[in.press_count++] = {arg, (arg & 1) != 0, (arg & 2) != 0, (arg & 4) != 0}; }
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

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0) {
        return 0;
    }
    const std::string_view rest{reinterpret_cast<const char*>(data + 1), size - 1};
    switch (data[0] % 7) {
    case 0: fuzz_config(rest); break;
    case 1: fuzz_theme(rest); break;
    case 2: fuzz_dock_layout(rest); break;
    case 3: fuzz_state(rest); break;
    case 4: fuzz_rich_text(rest); break;
    case 5: fuzz_chords(rest); break;
    default: fuzz_editing(rest); break;
    }
    return 0;
}
