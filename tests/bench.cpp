// Headless microbenchmark for strata: frame build time, steady-state heap
// allocations, idle detection, and virtualized table throughput.

#include "strata/backend/null.hpp"
#include "strata/strata.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#define STRATA_BENCH_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STRATA_BENCH_ASAN 1
#endif
#endif

#if !defined(STRATA_BENCH_ASAN)
namespace {

std::atomic<bool>          g_count_allocs{false};
std::atomic<std::uint64_t> g_alloc_count{0};

} // namespace

void* operator new(std::size_t size)
{
    if (g_count_allocs.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (size == 0) { size = 1; }
    void* p = std::malloc(size);
    if (!p) { std::abort(); }
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

void* operator new(std::size_t size, std::align_val_t al)
{
    if (g_count_allocs.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
    const std::size_t align = static_cast<std::size_t>(al);
    if (size == 0) { size = align; }
#if defined(_WIN32)
    void* p = _aligned_malloc(size, align);
#else
    const std::size_t rounded = (size + align - 1) & ~(align - 1);
    void* p = std::aligned_alloc(align, rounded);
#endif
    if (!p) { std::abort(); }
    return p;
}

void operator delete(void* p, std::align_val_t) noexcept
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete(void* p, std::size_t, std::align_val_t al) noexcept
{
    operator delete(p, al);
}
#endif // !STRATA_BENCH_ASAN

namespace {

using clock_t_ = std::chrono::steady_clock;

struct bench_result {
    const char*   name{};
    double        median_us{};
    double        mean_us{};
    std::uint32_t vertices{};
    std::uint32_t commands{};
    std::uint64_t allocs{};
    bool          can_idle{};
};

void reset_alloc_counter() noexcept
{
#if !defined(STRATA_BENCH_ASAN)
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_count_allocs.store(true, std::memory_order_release);
#endif
}

std::uint64_t stop_alloc_counter() noexcept
{
#if !defined(STRATA_BENCH_ASAN)
    g_count_allocs.store(false, std::memory_order_release);
    return g_alloc_count.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

template <typename Fn>
bench_result run_bench(const char* name, int warmup, int iters, Fn&& build_ui)
{
    strata::context_config cfg{};
    auto ui_opt = strata::context::create(cfg);
    if (!ui_opt) {
        std::fprintf(stderr, "strata_bench: failed to create context for %s\n", name);
        std::exit(1);
    }
    auto& ui = *ui_opt;
    ui.set_dock_animation(false);
    ui.set_scroll_smoothing(false);

    strata::null_renderer renderer;
    if (!renderer.create(ui.font())) {
        std::fprintf(stderr, "strata_bench: failed to create null_renderer\n");
        std::exit(1);
    }

    strata::input_state in{};
    in.display_size = {1280.0f, 720.0f};
    in.mouse_pos    = {-500.0f, -500.0f};
    in.delta_time   = 1.0f / 60.0f;

    for (int i = 0; i < warmup; ++i) {
        ui.begin_frame(in);
        build_ui(ui);
        ui.end_frame();
        renderer.render(ui.render_data());
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iters));

    reset_alloc_counter();
    for (int i = 0; i < iters; ++i) {
        const auto t0 = clock_t_::now();
        ui.begin_frame(in);
        build_ui(ui);
        ui.end_frame();
        renderer.render(ui.render_data());
        const auto t1 = clock_t_::now();
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        samples.push_back(us);
    }
    const std::uint64_t allocs = stop_alloc_counter();

    double sum = 0.0;
    for (double s : samples) { sum += s; }
    std::sort(samples.begin(), samples.end());
    const double median = samples[samples.size() / 2];

    const auto& st = renderer.last();
    return {
        .name      = name,
        .median_us = median,
        .mean_us   = sum / static_cast<double>(samples.size()),
        .vertices  = st.vertices,
        .commands  = st.commands,
        .allocs    = allocs,
        .can_idle  = ui.can_idle(),
    };
}

} // namespace

int main(int argc, char** argv)
{
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) { quick = true; }
    }

    const int warmup = 10;
    const int iters  = quick ? 40 : 400;

    // 1. Typical inspector panel (~25 mixed controls)
    bool  enabled = true;
    bool  vsync   = true;
    bool  wire    = false;
    float exposure = 1.25f;
    float gamma    = 2.2f;
    float bloom    = 0.35f;
    int   quality  = 2;
    static constexpr std::string_view k_qualities[] = {"Low", "Medium", "High", "Ultra"};

    const auto r_inspector = run_bench("inspector_25_widgets", warmup, iters, [&](strata::context& ui) {
        if (auto w = ui.window("Inspector", {24, 24}, {340, 480})) {
            ui.text("Render Pipeline");
            ui.separator();
            ui.checkbox("Enabled", enabled);
            ui.checkbox("V-Sync", vsync);
            ui.checkbox("Wireframe", wire);
            ui.slider("Exposure", exposure, 0.1f, 4.0f);
            ui.slider("Gamma", gamma, 1.0f, 3.0f);
            ui.slider("Bloom", bloom, 0.0f, 1.0f);
            ui.combo("Quality", quality, k_qualities);
            ui.separator();
            for (int i = 0; i < 12; ++i) {
                ui.push_id(i);
                ui.text("Channel");
                ui.same_line();
                ui.button("Reset");
                ui.pop_id();
            }
        }
    });

    // 2. Dense 1,000 interactive controls in a scrollable window
    std::vector<bool>  flags(500, false);
    std::vector<float> vals(500, 0.5f);

    const auto r_dense = run_bench("dense_1k_widgets", warmup, iters, [&](strata::context& ui) {
        if (auto w = ui.window("Dense Controls", {24, 24}, {520, 640})) {
            for (int i = 0; i < 500; ++i) {
                ui.push_id(i);
                bool b = flags[static_cast<std::size_t>(i)];
                if (ui.checkbox("Active", b)) { flags[static_cast<std::size_t>(i)] = b; }
                ui.same_line();
                ui.slider("Weight", vals[static_cast<std::size_t>(i)], 0.0f, 1.0f);
                ui.pop_id();
            }
        }
    });

    // 3. Virtualized 100,000-row table via list_clipper
    const auto r_table = run_bench("table_100k_rows", warmup, iters, [&](strata::context& ui) {
        if (auto w = ui.window("Telemetry Table", {24, 24}, {760, 560})) {
            if (auto t = ui.table("rows", 4, strata::table_default, 480.0f)) {
                ui.table_setup_column("ID", 90.0f);
                ui.table_setup_column("Name");
                ui.table_setup_column("Category", 120.0f);
                ui.table_setup_column("Value", 100.0f);
                (void)ui.table_headers_row();

                strata::list_clipper clip(ui, 100000);
                while (clip.step()) {
                    for (int row = clip.begin(); row < clip.end(); ++row) {
                        if (ui.table_next_row()) {
                            ui.table_next_column();
                            ui.textf("#{}", row);
                            ui.table_next_column();
                            ui.textf("entity_{}", row);
                            ui.table_next_column();
                            ui.text((row & 1) ? "render" : "physics");
                            ui.table_next_column();
                            ui.textf("{:.2f}", static_cast<float>(row % 1000) * 0.01f);
                        }
                    }
                }
            }
        }
    });

    // 4. Multi-window docked-style workspace (4 panels, ~80 controls)
    const auto r_workspace = run_bench("workspace_4_panels", warmup, iters, [&](strata::context& ui) {
        if (auto w = ui.window("Hierarchy", {16, 16}, {260, 680})) {
            for (int i = 0; i < 24; ++i) {
                ui.push_id(i);
                ui.button("Node");
                ui.pop_id();
            }
        }
        if (auto w = ui.window("Viewport", {288, 16}, {640, 440})) {
            ui.text("1280x720  60.0 fps");
            ui.separator();
            ui.slider("Exposure", exposure, 0.1f, 4.0f);
        }
        if (auto w = ui.window("Console", {288, 468}, {640, 228})) {
            for (int i = 0; i < 16; ++i) {
                ui.text("info: frame ok");
            }
        }
        if (auto w = ui.window("Properties", {940, 16}, {320, 680})) {
            ui.checkbox("Enabled", enabled);
            ui.checkbox("V-Sync", vsync);
            ui.slider("Gamma", gamma, 1.0f, 3.0f);
            ui.slider("Bloom", bloom, 0.0f, 1.0f);
            ui.combo("Quality", quality, k_qualities);
        }
    });

    const bench_result results[] = {r_inspector, r_dense, r_table, r_workspace};

    std::printf("strata headless benchmark (%d steady-state frames each, warmup %d)\n\n", iters, warmup);
    std::printf("| Scenario | Median (us) | Mean (us) | Vertices | Draw Cmds | Heap Allocs | Idle |\n");
    std::printf("|---|---:|---:|---:|---:|---:|---|\n");

    bool ok = true;
    for (const auto& r : results) {
        std::printf("| `%s` | %.1f | %.1f | %u | %u | %llu | %s |\n",
            r.name, r.median_us, r.mean_us, r.vertices, r.commands,
            static_cast<unsigned long long>(r.allocs),
            r.can_idle ? "yes" : "no");
        if (r.allocs != 0) { ok = false; }
        if (!r.can_idle) { ok = false; }
    }

    if (!ok) {
        std::fprintf(stderr, "\nstrata_bench: invariant failed (expected 0 steady-state allocs and can_idle() == true)\n");
        return 1;
    }
    return 0;
}
