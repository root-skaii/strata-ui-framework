#pragma once

// an in-game overlay for direct3d 11 and 12 applications: hooks the game's swap chain, draws a strata ui on top of every
// frame and takes the keyboard / mouse while it is open. meant to live in a dll loaded into the game; overlay/demo shows
// the whole thing.
//
//   strata::overlay::options opt;
//   opt.ui = [](strata::context& ui) { if (auto w = ui.window("my tool", {40, 40}, {360, 0})) { ui.text("hello"); } };
//   strata::overlay::install(opt);        // from a thread of your own, NOT from DllMain (it loads dxgi / d3d11 and waits)
//
// how it works: a dummy swap chain yields IDXGISwapChain's vtable (shared by every swap chain of that implementation);
// Present, Present1 and ResizeBuffers are replaced in it, no code is patched. the first swap chain that presents with a
// real window becomes the game's: the ui is drawn into its back buffer just before Present, and the window is subclassed
// to read input. nothing is drawn while the overlay is hidden.
// direct3d 12: the same trick on ID3D12CommandQueue's ExecuteCommandLists; the direct queue seen executing command lists
// is taken as the game's. the overlay records its own command list per frame and submits it on that queue right before
// Present, with a fence so the frame's allocator and upload buffers are reused only once the gpu is done with them.
//
// limits: only direct3d 11 / 12. a d3d12 game with several direct queues is served by the one that ran last before
// Present. a game that recenters or clips the cursor (mouse-look) fights the overlay for it: the overlay unclips it every
// frame while open, but a game that locks it in relative mode has to be told to let go (Unity: Cursor.lockState = None).
// do not use it in online games with anti-cheat.

#include <strata/strata.hpp>

#include <atomic>
#include <functional>
#include <string>

namespace strata::overlay {

struct options {
    // called every frame the overlay is visible, between begin_frame and end_frame, on the game's render thread
    std::function<void(context&)> ui;
    // virtual-key code that shows / hides the overlay (default F1); 0: none (use show())
    unsigned toggle_key = 0x70;
    bool     start_visible = false;
    // while visible the game does not see the keyboard and mouse (the ui decides what it wants when this is false: only what
    // the pointer is over, and the keyboard while a text field is focused)
    bool     block_game_input = true;
    // changes the context_config (fonts, theme) before the context is created
    std::function<void(context_config&)> configure_context;
    // called once, on the render thread, after the context exists and before its first frame
    std::function<void(context&)>       on_ready;
    // where the ui scale starts. 0 = the monitor's dpi scale, which is only a starting point: an overlay is often
    // wanted a little smaller or larger than the desktop is. set_ui_scale() changes it later.
    float    ui_scale = 0.0f;
    // Ctrl + Plus / Ctrl + Minus step the ui scale by 10 %, Ctrl + 0 puts it back to `ui_scale`, while the overlay
    // has the keyboard. Off by default so it cannot collide with the host's own shortcuts.
    bool     scale_hotkeys = false;
    // debug: write the back buffer (with the ui drawn) to this png after `capture_frame` visible frames, once
    std::string capture_path;
    unsigned    capture_frame = 30;
};

// starts the hook and returns once it is installed (false: it could not be, see last_error()). safe to call once.
bool install(const options& opt);
// takes the hook out again and frees everything; the dll may be unloaded afterwards. the game can be inside a hooked call
// on another thread for a moment: uninstall() waits a little for that.
void uninstall();

void show(bool visible);
[[nodiscard]] bool visible() noexcept;
// frames the overlay has drawn (0 while hidden / not attached yet): handy to check that it works
[[nodiscard]] unsigned long long frames() noexcept;
[[nodiscard]] const char* last_error() noexcept;
// true once the hook has met the game's swap chain and built the ui on it (the first Present after install())
[[nodiscard]] bool attached() noexcept;
// the swap chain's window, once attached (HWND), nullptr before
[[nodiscard]] void* window() noexcept;

// the ui scale, as a factor (1.0 = one logical pixel per physical pixel). Changing it rebuilds the font atlas and
// re-uploads it to the renderer, which only the render thread may do, so the new value is applied at the start of the
// next frame -- set_ui_scale() may be called from any thread and returns immediately. Everything the ui draws gets
// bigger or smaller together; nothing about the game's own window changes.
void set_ui_scale(float scale) noexcept;
[[nodiscard]] float ui_scale() noexcept;
// ... and in percent, which is what a setting shows. 50 .. 400, clamped.
inline void set_ui_scale_percent(int percent) noexcept { set_ui_scale(static_cast<float>(percent) / 100.0f); }
[[nodiscard]] inline int ui_scale_percent() noexcept { return static_cast<int>(ui_scale() * 100.0f + 0.5f); }

} // namespace strata::overlay
