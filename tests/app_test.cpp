// strata::app with a real window and device: frames, quitting, device reset, vetoed close, idling, state file.
//   strata_app_test.exe     exit code 0 = passed

#include <strata/app.hpp>
#include <strata/config.hpp>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace {

int g_failures = 0;
int g_checks   = 0;

void check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL  line %d: %s\n", line, what);
    }
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

void test_run_and_reset(const std::string& state_file)
{
    std::fprintf(stderr, "[app: frames, reset_device, a vetoed close, quit, the state file]\n");
    auto app = strata::app::create({.title = "strata app test", .width = 640, .height = 400, .vsync = false, .idle = false,
                                    .state_file = state_file});
    CHECK(app.has_value());
    if (!app) { return; }
    int resets = 0, close_requests = 0;
    app->on_device_reset = [&](strata::app&) { ++resets; };
    app->on_close_request = [&](strata::app&) { ++close_requests; return false; }; // "unsaved changes"
    int frame = 0;
    const int code = app->run([&](strata::app& a, strata::context& ui) {
        if (auto w = ui.window("hello", {20, 20}, 240.0f)) { ui.textf("frame {}", frame); }
        ++frame;
        if (frame == 10) { CHECK(a.reset_device()); }                        // the path a lost device takes
        if (frame == 20) { ::PostMessageW(static_cast<HWND>(a.window()), WM_CLOSE, 0, 0); } // vetoed: it keeps running
        if (frame == 40) { a.quit(7); }
    });
    CHECK(code == 7);
    CHECK(frame == 40);
    CHECK(resets == 1);
    CHECK(close_requests == 1);
    CHECK(app->frames_drawn() >= 30);        // (idling off: every frame drawn, apart from the reset)
    CHECK(app->device() != nullptr);

    strata::config state;
    CHECK(state.load_file(state_file));
    CHECK(state.get("ui", "version") == "1");
}

void test_idle()
{
    std::fprintf(stderr, "[app: a ui nobody touches sleeps: a handful of frames in half a second]\n");
    auto app = strata::app::create({.title = "strata idle test", .width = 400, .height = 300, .idle = true});
    CHECK(app.has_value());
    if (!app) { return; }
    const HWND hwnd = static_cast<HWND>(app->window());
    std::thread closer([hwnd] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    });
    const auto start = std::chrono::steady_clock::now();
    const int code = app->run([&](strata::app&, strata::context& ui) {
        if (auto w = ui.window("static", {20, 20}, 240.0f)) { ui.text("nothing moves"); } // (the pointer is not over it)
    });
    closer.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(code == 0);
    CHECK(seconds >= 0.45);                 // it waited for the close
    CHECK(app->frames() < 30);             // ... without spinning (at 60 fps it would be ~30, unthrottled thousands)
    CHECK(app->frames_drawn() < app->frames() + 1);
}

} // namespace

int main()
{
    const std::string state_file = (std::filesystem::temp_directory_path() / "strata_app_test.ini").string();
    std::filesystem::remove(state_file);
    test_run_and_reset(state_file);
    test_idle();
    std::filesystem::remove(state_file);
    std::fprintf(stderr, "app test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
