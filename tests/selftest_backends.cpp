// the null and software renderers: concept conformance, texture lifecycle and what the rasteriser puts on screen

#include "selftest_common.hpp"

#include <strata/backend/null.hpp>
#include <strata/backend/software.hpp>

static_assert(renderer_backend<null_renderer>);
static_assert(renderer_backend<software_renderer>);

namespace {

constexpr std::array<u8, 16> red2x2 = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};

[[nodiscard]] std::array<u8, 4> pixel(const software_renderer& r, u32 x, u32 y)
{
    const auto px = r.pixels();
    const std::size_t i = (static_cast<std::size_t>(y) * r.width() + x) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

void test_null_renderer()
{
    std::fprintf(stderr, "[null renderer: counts and validates draw data]\n");
    harness h;
    null_renderer r;
    CHECK(!r.valid());
    CHECK(r.create(h.ui.font()) && r.valid());

    const texture_id tex = r.create_texture(2, 2, red2x2);
    CHECK(tex != 0);
    CHECK(r.create_texture(2, 2, std::span<const u8>{red2x2.data(), 3}) == 0); // too few bytes
    CHECK(!r.update_texture(tex, 0, 0, 1, 1, red2x2)); // not updatable

    h.frame([&] {
        if (auto w = h.ui.window("w", {10, 10}, {200, 120})) {
            h.ui.button("ok");
            h.ui.text("hello");
        }
        h.ui.draw().image({{300, 300}, {340, 340}}, tex);
    });
    r.render(h.ui.render_data());
    CHECK(r.last().frames == 1);
    CHECK(r.last().commands > 0 && r.last().vertices > 0 && r.last().indices > 0);
    CHECK(r.last().textured == 1);
    CHECK(r.last().well_formed);

    r.destroy_texture(tex);
    r.render(h.ui.render_data()); // the same frame now names a texture that is gone
    CHECK(!r.last().well_formed);
    CHECK(r.last().frames == 2);
}

void test_software_renderer()
{
    std::fprintf(stderr, "[software renderer: rasterised output]\n");
    harness h;
    software_renderer r;
    CHECK(!r.valid());
    CHECK(r.create(h.ui.font()) && r.valid());

    const texture_id tex = r.create_texture(2, 2, red2x2);
    CHECK(tex != 0);

    h.frame([&] {
        draw_list& dl = h.ui.draw();
        dl.rect_filled({{10, 10}, {50, 50}}, {0, 200, 0, 255});
        dl.push_clip({{100, 100}, {120, 120}});
        dl.rect_filled({{90, 90}, {140, 140}}, {0, 0, 255, 255}); // only the clipped part may land
        dl.pop_clip();
        dl.image({{200, 200}, {240, 240}}, tex);
        dl.rect_filled({{300, 300}, {340, 340}}, {255, 255, 255, 128}); // half transparent over the clear colour
        dl.text({400, 400}, {255, 255, 255, 255}, "Hello");
    });

    r.resize(800, 600);
    r.clear({0, 0, 0, 255});
    r.render(h.ui.render_data());
    CHECK(r.width() == 800 && r.height() == 600);
    CHECK(r.pixels().size() == 800u * 600u * 4u);

    CHECK(pixel(r, 30, 30) == (std::array<u8, 4>{0, 200, 0, 255}));
    CHECK(pixel(r, 5, 5) == (std::array<u8, 4>{0, 0, 0, 255})); // untouched
    CHECK(pixel(r, 110, 110) == (std::array<u8, 4>{0, 0, 255, 255}));
    CHECK(pixel(r, 95, 95) == (std::array<u8, 4>{0, 0, 0, 255})); // outside the clip
    CHECK(pixel(r, 125, 125) == (std::array<u8, 4>{0, 0, 0, 255}));
    CHECK(pixel(r, 220, 220) == (std::array<u8, 4>{255, 0, 0, 255})); // the image
    const auto half = pixel(r, 320, 320);
    CHECK(half[0] >= 127 && half[0] <= 129 && half[3] == 255);

    bool any_text = false;
    for (u32 y = 400; y < 420 && !any_text; ++y) {
        for (u32 x = 400; x < 450; ++x) {
            any_text = any_text || pixel(r, x, y)[0] > 100;
        }
    }
    CHECK(any_text);

    // no pixel on a shared triangle edge is blended twice: a translucent quad is uniform, diagonal included
    for (u32 i = 0; i < 40; ++i) {
        CHECK(pixel(r, 300 + i, 300 + i)[0] == half[0]);
    }

    std::vector<u8> straight(r.pixels().size());
    CHECK(r.copy_straight(straight));
    CHECK(straight[(30 * 800 + 30) * 4 + 1] == 200);
    std::vector<u8> small(4);
    CHECK(!r.copy_straight(small));

    // a transparent clear stays transparent where nothing is drawn
    r.clear();
    r.render(h.ui.render_data());
    CHECK(pixel(r, 5, 5)[3] == 0);
    CHECK(pixel(r, 30, 30)[3] == 255);

    // textures: updatable ones change, others refuse
    const texture_id dyn = r.create_texture(texture_desc{2, 2, texture_format::rgba8, 1, true}, red2x2);
    const std::array<u8, 4> green{0, 255, 0, 255};
    CHECK(r.update_texture(dyn, 0, 0, 1, 1, green));
    CHECK(!r.update_texture(tex, 0, 0, 1, 1, green));
    CHECK(!r.update_texture(dyn, 2, 0, 1, 1, green)); // out of bounds
    r.destroy_texture(dyn);
    CHECK(!r.update_texture(dyn, 0, 0, 1, 1, green));
}

void test_software_ui_frame()
{
    std::fprintf(stderr, "[software renderer: a whole ui frame]\n");
    harness h;
    software_renderer r;
    CHECK(r.create(h.ui.font()));

    const auto build = [&] {
        if (auto w = h.ui.window("panel", {20, 20}, {300, 200})) {
            h.ui.text("label");
            h.ui.button("press");
        }
    };
    h.frame(build);
    r.resize(800, 600);
    r.clear({30, 30, 34, 255});
    r.render(h.ui.render_data());

    std::size_t changed = 0;
    const auto px = r.pixels();
    for (std::size_t i = 0; i < px.size(); i += 4) {
        changed += (px[i] != 30 || px[i + 1] != 30 || px[i + 2] != 34) ? 1u : 0u;
    }
    CHECK(changed > 5000); // the window body, its border and text
    CHECK(pixel(r, 700, 500) == (std::array<u8, 4>{30, 30, 34, 255})); // far from the window
}

} // namespace

void run_backend_tests()
{
    test_null_renderer();
    test_software_renderer();
    test_software_ui_frame();
}
