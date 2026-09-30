// headless checks of overlay::host / module: lifecycle, modes, ordering, enable / remove. no window, no hook, no input.

#include <strata/overlay/module.hpp>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

#define CHECK(...)                                                                                   \
    do {                                                                                             \
        if (!(__VA_ARGS__)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); ++failures; } \
    } while (0)

using strata::overlay::module_mode;

std::string trace; // "<name><event>" in the order things happened: a attach, f frame, d detach

struct probe final : strata::overlay::module {
    probe(char id, module_mode m) : id_{id}, mode_{m} {}
    ~probe() override = default;
    void on_attach(strata::context&) override { trace += id_; trace += 'a'; }
    void on_frame(strata::context&) override { trace += id_; trace += 'f'; }
    void on_detach() override { trace += id_; trace += 'd'; }
    [[nodiscard]] module_mode mode() const noexcept override { return mode_; }
    char        id_;
    module_mode mode_;
};

// one ui frame with the host's modules inside it
void run(strata::overlay::host& h, strata::context& ui, bool open)
{
    strata::input_state in;
    in.display_size = {800.0f, 600.0f};
    in.delta_time   = 1.0f / 60.0f;
    ui.begin_frame(in);
    h.frame(ui, open);
    ui.end_frame();
}

} // namespace

int main()
{
    auto ui = strata::context::create().value();

    { // modes: `always` runs closed or open, `when_open` only while open; registration order is run order
        trace.clear();
        strata::overlay::host h;
        h.add<probe>('m', module_mode::when_open);
        h.add<probe>('h', module_mode::always);
        run(h, ui, false);
        CHECK(trace == "mahahf"); // both attach on the first frame; only the `always` one runs
        trace.clear();
        run(h, ui, true);
        CHECK(trace == "mfhf");
        trace.clear();
        run(h, ui, false);
        CHECK(trace == "hf");
    }
    CHECK(trace == "hfmdhd"); // destroying the host detaches what was attached, in order

    { // enabled: no frames, state kept, attaches once
        trace.clear();
        strata::overlay::host h;
        auto& p = h.add<probe>('p', module_mode::always);
        run(h, ui, true);
        p.set_enabled(false);
        run(h, ui, true);
        CHECK(trace == "papf");
        p.set_enabled(true);
        run(h, ui, true);
        CHECK(trace == "papfpf");
    }

    { // remove (host not installed: at once; an installed host detaches on the render thread's next frame)
        trace.clear();
        strata::overlay::host h;
        auto& a = h.add<probe>('a', module_mode::always);
        run(h, ui, true);
        h.remove(a);
        h.add<probe>('b', module_mode::always);
        run(h, ui, true);
        CHECK(trace == "aaafadbabf"); // not installed: a detaches at remove(); b attaches on the next frame, then runs
        trace.clear();
        run(h, ui, true);
        CHECK(trace == "bf");
    }

    { // nothing installed: remove is immediate and never attached modules are not detached
        trace.clear();
        strata::overlay::host h;
        auto& p = h.add<probe>('p', module_mode::always);
        h.remove(p);
        CHECK(trace.empty());
        run(h, ui, true);
        CHECK(trace.empty());
        CHECK(!h.installed());
    }

    std::printf("%s\n", failures == 0 ? "overlay modules: ok" : "overlay modules: FAILED");
    return failures == 0 ? 0 : 1;
}
