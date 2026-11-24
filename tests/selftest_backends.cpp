// the null and software renderers: concept conformance, texture lifecycle and what the rasteriser puts on screen

#include "selftest_common.hpp"

#include <strata/backend/null.hpp>
#include <strata/backend/software.hpp>
#include <strata/svg.hpp>
#ifdef _WIN32
#include <strata/platform/win32.hpp>
#include <windows.h>
#endif
#if STRATA_HAS_OPENGL
#include <strata/backend/opengl.hpp>
static_assert(renderer_backend<gl_renderer>);
#endif
#if STRATA_HAS_VULKAN
#include <strata/backend/vulkan.hpp>
static_assert(renderer_backend<vulkan_renderer>);
#endif

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

void test_gpu_backend_shells()
{
    std::fprintf(stderr, "[gpu backends: uninitialised lifecycle & concept checks]\n");
#if STRATA_HAS_OPENGL
    {
        gl_renderer gl;
        CHECK(!gl.valid());
        CHECK(!gl.device_lost());
        CHECK(gl.create_texture(2, 2, red2x2) == 0);
        gl.destroy();
    }
#endif
#if STRATA_HAS_VULKAN
    {
        vulkan_renderer vk;
        CHECK(!vk.valid());
        CHECK(!vk.device_lost());
        CHECK(vk.create_texture(2, 2, red2x2) == 0);
        vk.clear_target_cache();
        vk.destroy();
    }
#endif
}

void test_rendering_and_input_regressions()
{
    std::fprintf(stderr, "[regressions: origin, merge_commands, rect_outline, sdf, svg, win32 click, dnd rect]\n");
    harness h;

    // 1. software_renderer honors draw_data::origin for vertices, clip, and SDF shapes
    {
        software_renderer r;
        CHECK(r.create(h.ui.font()));
        draw_list dl;
        dl.begin({64.0f, 64.0f}, h.ui.font());
        dl.push_clip_absolute({{100.0f, 200.0f}, {164.0f, 264.0f}});
        dl.rect_filled({{110.0f, 210.0f}, {150.0f, 250.0f}}, {200, 60, 40, 255}, 6.0f);
        dl.pop_clip();
        draw_data dd = dl.data();
        dd.origin = {100.0f, 200.0f};
        r.resize(64, 64);
        r.clear({0, 0, 0, 255});
        r.render(dd);
        const auto center = pixel(r, 30, 30);
        CHECK(center[0] > 180 && center[1] < 80 && center[2] < 60);
        CHECK(pixel(r, 2, 2) == (std::array<u8, 4>{0, 0, 0, 255}));
    }

    // 2. draw_list::merge_commands preserves *barrier == 0 when n >= 2
    {
        draw_list dl;
        dl.begin({200.0f, 200.0f}, h.ui.font());
        dl.rect_filled({{0.0f, 0.0f}, {20.0f, 20.0f}}, {255, 0, 0, 255});
        dl.break_command();
        dl.rect_filled({{30.0f, 30.0f}, {50.0f, 50.0f}}, {0, 255, 0, 255});
        u32 barrier = 0;
        dl.merge_commands(&barrier);
        CHECK(barrier == 0);
    }

    // 3. draw_list::rect_outline fast path: thickness <= 0 emits nothing; thick outline stays inside bounds
    {
        draw_list dl;
        dl.begin({100.0f, 100.0f}, h.ui.font());
        dl.rect_outline({{10.0f, 10.0f}, {40.0f, 40.0f}}, {255, 255, 255, 255}, 0.0f, 0.0f);
        CHECK(dl.data().indices.empty());

        software_renderer r;
        CHECK(r.create(h.ui.font()));
        dl.begin({64.0f, 64.0f}, h.ui.font());
        dl.rect_outline({{10.0f, 10.0f}, {30.0f, 20.0f}}, {100, 100, 100, 128}, 0.0f, 25.0f);
        r.resize(64, 64);
        r.clear({0, 0, 0, 255});
        r.render(dl.data());
        CHECK(pixel(r, 20, 5) == (std::array<u8, 4>{0, 0, 0, 255}));
        CHECK(pixel(r, 20, 25) == (std::array<u8, 4>{0, 0, 0, 255}));
    }

    // 4. SDF font_atlas preserves is_sdf / sdf_spread across copy, assign, save/load, and dynamic bake
    {
        static constexpr codepoint_range ascii_only[] = {{0x20, 0x7e}};
        font_config cfg;
        cfg.face         = "Segoe UI";
        cfg.pixel_height = 18.0f;
        cfg.sdf          = true;
        cfg.sdf_spread   = 4;
        cfg.ranges       = ascii_only;
        auto built = font_atlas::build(cfg, 4096, 1.0f, true);
        CHECK(built.has_value());
        if (built) {
            CHECK(built->is_sdf() && built->sdf_spread() == 4);
            const font_atlas copy_ctor{*built};
            CHECK(copy_ctor.is_sdf() && copy_ctor.sdf_spread() == 4);
            font_atlas copy_assign;
            copy_assign = *built;
            CHECK(copy_assign.is_sdf() && copy_assign.sdf_spread() == 4);
            const std::vector<u8> blob = built->save();
            const auto loaded = font_atlas::load(blob);
            CHECK(loaded.has_value() && loaded->is_sdf() && loaded->sdf_spread() == 4);
            // dynamic glyph outside ASCII should be SDF-padded
            if (built->bake_glyph(0, U'\u00E9')) {
                const auto& g = built->find(0, U'\u00E9');
                CHECK(g.present && g.visible);
                CHECK((g.x1 - g.x0) >= 8.0f);
            }
        }
    }

    // 5. SVG parsing after Z, unclosed fill closing edge, and seamless convex draw_svg_path
    {
        const auto contours = svg_parse_path("M0 0 H10 V10 H0 Z L20 20");
        CHECK(contours.size() == 2);
        CHECK(contours[0].closed && contours[0].points.size() == 4);
        CHECK(!contours[1].closed && contours[1].points.size() == 2);

        const auto alpha = svg_rasterize_alpha("M2 2 H14 V14 H2", {16.0f, 16.0f}, 16, 16);
        CHECK(alpha[8 * 16 + 8] == 255);

        software_renderer r;
        CHECK(r.create(h.ui.font()));
        draw_list dl;
        dl.begin({32.0f, 32.0f}, h.ui.font());
        draw_svg_path(dl, {{4.0f, 4.0f}, {28.0f, 28.0f}}, svg_icons::stop, svg_icons::viewbox, {255, 255, 255, 255});
        r.resize(32, 32);
        r.clear({0, 0, 0, 255});
        r.render(dl.data());
        // diagonal center of stop icon must be solid opaque white (no translucent fan seam)
        CHECK(pixel(r, 16, 16) == (std::array<u8, 4>{255, 255, 255, 255}));
    }

#ifdef _WIN32
    // 6. Win32 button press/release updates mouse_pos even after WM_MOUSELEAVE
    {
        win32_platform plat;
        plat.handle_message(nullptr, WM_MOUSELEAVE, 0, 0);
        plat.handle_message(nullptr, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(123, 234));
        const input_state in1 = plat.new_frame();
        CHECK(in1.mouse_pos.x == 123.0f && in1.mouse_pos.y == 234.0f && in1.mouse_down[0]);
        plat.handle_message(nullptr, WM_LBUTTONUP, 0, MAKELPARAM(125, 236));
        const input_state in2 = plat.new_frame();
        CHECK(in2.mouse_pos.x == 125.0f && in2.mouse_pos.y == 236.0f && !in2.mouse_down[0]);
    }
#endif

    // 7. Drag-source preview does not clobber item_rect() for drop_target() on the dragged item
    {
        rect before_rect{};
        rect after_rect{};
        const auto build = [&] {
            if (auto w = h.ui.window("dnd", {0, 0}, {300, 200}, plain_window)) {
                (void)h.ui.selectable("row0", false);
                before_rect = h.ui.item_rect();
                if (auto d = h.ui.drag_source("k", 1)) {
                    h.ui.text("preview");
                }
                after_rect = h.ui.item_rect();
            }
        };
        h.frames(build, 2);
        const vec2 start = {before_rect.min.x + 12.0f, before_rect.min.y + 8.0f};
        h.move(start);
        h.frames(build, 2);
        h.down();
        h.frame(build);
        h.move(start + vec2{20.0f, 0.0f});
        h.frames(build, 2);
        CHECK(after_rect.min.x == before_rect.min.x && after_rect.min.y == before_rect.min.y &&
              after_rect.max.x == before_rect.max.x && after_rect.max.y == before_rect.max.y);
        h.up();
        h.frame(build);
    }

    // 8. Single-frame jump from offscreen directly into a button press activates and clicks on release
    {
        rect btn_rect{};
        int  clicks = 0;
        const auto build = [&] {
            if (auto w = h.ui.window("tap", {40, 40}, {240, 120}, plain_window)) {
                if (h.ui.button("tap me")) { ++clicks; }
                btn_rect = h.ui.item_rect();
            }
        };
        h.move({-1.0e6f, -1.0e6f});
        h.frames(build, 2);
        const vec2 center = btn_rect.center();
        h.move(center);
        h.down();
        h.frame(build);
        h.up();
        h.frame(build);
        CHECK(clicks == 1);
    }
}

} // namespace

void run_backend_tests()
{
    test_null_renderer();
    test_software_renderer();
    test_software_ui_frame();
    test_gpu_backend_shells();
    test_rendering_and_input_regressions();
}
