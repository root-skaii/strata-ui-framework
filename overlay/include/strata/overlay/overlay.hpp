#pragma once

// in-game overlay for direct3d 11 / 12: hooks the game's swap chain, draws a strata ui on every frame and takes
// keyboard / mouse while open. lives in a dll loaded into the game; see overlay/demo.
//
//   strata::overlay::options opt;
//   opt.ui = [](strata::context& ui) { if (auto w = ui.window("my tool", {40, 40}, {360, 0})) { ui.text("hello"); } };
//   strata::overlay::install(opt);        // from your own thread, NOT DllMain (it loads dxgi / d3d11 and waits)
//
// how: a dummy swap chain (made with the api the game already loaded) yields IDXGISwapChain's shared vtable, whose
// Present, Present1 and ResizeBuffers entries are replaced; no code is patched. the first swap chain presenting to a
// real window is the game's: the ui is drawn into its back buffer before Present and the window is subclassed for
// input. nothing is drawn while hidden.
// d3d12: ID3D12CommandQueue::ExecuteCommandLists is hooked the same way; the direct queue seen executing is the
// game's. the overlay submits its own command list on it before Present, fenced so per-frame allocators and upload
// buffers are reused only when the gpu is done.
//
// limits: d3d11 / 12 only. with several direct queues, the last one before Present is used. games that lock the
// cursor in relative mode must release it (Unity: Cursor.lockState = None); the overlay unclips it each frame.
// do not use in online games with anti-cheat.

#include <strata/strata.hpp>

#include <atomic>
#include <functional>
#include <optional>
#include <string>

namespace strata::overlay {

struct options {
    // called every visible frame between begin_frame and end_frame, on the game's render thread
    std::function<void(context&)> ui;
    // virtual-key that toggles the overlay (default F1); 0: none (use show())
    unsigned toggle_key = 0x70;
    bool     start_visible = false;
    // while visible the game gets no keyboard / mouse (false: the ui takes only what it is over, and keys while typing)
    bool     block_game_input = true;
    // edits the context_config (fonts, theme) before the context is created
    std::function<void(context_config&)> configure_context;
    // called once on the render thread, before the first frame
    std::function<void(context&)>       on_ready;
    // initial ui scale; 0 = the monitor's dpi scale. set_ui_scale() changes it later.
    float    ui_scale = 0.0f;
    // Ctrl + Plus / Minus step the scale by 10 %, Ctrl + 0 resets to `ui_scale` (while the overlay has the keyboard).
    // off by default to avoid clashing with host shortcuts.
    bool     scale_hotkeys = false;
    // colour encoding into the game's frame. empty = from the swap chain: FP16 -> scRGB, 10-bit -> HDR10 if the game
    // declared it (or the monitor is in hdr mode), else srgb. set it when detection is wrong
    std::optional<output_space> output;
    // hdr ui white in nits; only while the monitor is in hdr mode (scRGB on an sdr monitor uses the display's white)
    float    hdr_paper_white_nits = 200.0f;
    // debug: saves the back buffer (with ui) to this png once, after `capture_frame` visible frames
    std::string capture_path;
    unsigned    capture_frame = 30;
};

// installs the hook and returns (false: see last_error()). call once.
bool install(const options& opt);
// removes the hook and frees everything; the dll may then be unloaded. waits briefly for hooked calls in flight.
void uninstall();

void show(bool visible);
[[nodiscard]] bool visible() noexcept;
// frames drawn (0 while hidden / not attached)
[[nodiscard]] unsigned long long frames() noexcept;
[[nodiscard]] const char* last_error() noexcept;
// current output encoding (see options::output)
[[nodiscard]] output_space output_space_in_use() noexcept;
// the hook has met the game's swap chain and built the ui (first Present after install())
[[nodiscard]] bool attached() noexcept;
// the swap chain's HWND once attached, else nullptr
[[nodiscard]] void* window() noexcept;

// ui scale factor (1.0 = 1:1). callable from any thread; the atlas is rebuilt on the render thread next frame.
void set_ui_scale(float scale) noexcept;
[[nodiscard]] float ui_scale() noexcept;
// ... in percent, clamped to 50 .. 400.
inline void set_ui_scale_percent(int percent) noexcept { set_ui_scale(static_cast<float>(percent) / 100.0f); }
[[nodiscard]] inline int ui_scale_percent() noexcept { return static_cast<int>(ui_scale() * 100.0f + 0.5f); }

} // namespace strata::overlay
