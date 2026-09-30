// headless self-test: images, tables, charts, number fields, tabs, date pickers, lists, scrollbars

#include "selftest_common.hpp"

namespace {

void test_images()
{
    std::fprintf(stderr, "[images]\n");
    harness h;
    const auto build = [&] {
        if (auto w = h.ui.window("img", {20, 20}, {400, 0}, plain_window)) {
            h.ui.image(7, {40, 40});
            h.ui.text("text between");
            h.ui.image(7, {0, 0}, {0.25f, 0.25f}, {0.75f, 0.75f}, {255, 128, 128, 255}, 8.0f);
            (void)h.ui.image_button("btn", 9, {32, 32});
            h.ui.image(0, {40, 40}); // texture 0 is "none": nothing is drawn
        }
    };
    h.frames(build, 2);
    const draw_data d = h.ui.render_data();
    int with7 = 0, with9 = 0;
    for (const draw_cmd& c : d.commands) {
        with7 += c.texture == 7 && c.idx_count > 0;
        with9 += c.texture == 9 && c.idx_count > 0;
    }
    CHECK(with7 == 2);
    CHECK(with9 >= 1);
    // an image command holds only image quads
    for (const draw_cmd& c : d.commands) {
        if (c.texture != 0) { CHECK(c.idx_count % 6 == 0); }
    }
}

// nested tables ------------------------------------------------------------------------------------------------

void test_nested_tables()
{
    std::fprintf(stderr, "[nested tables]\n");
    harness h;
    f32 after_y[3]{};
    bool inner_ok = true;
    bool too_deep_refused = false;
    int  frame_no = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("tables", {0, 0}, {500, 0}, plain_window)) {
            if (h.ui.begin_table("outer", 2)) {
                h.ui.table_setup_column("Name");
                h.ui.table_setup_column("Details");
                (void)h.ui.table_headers_row();
                for (int r = 0; r < 3; ++r) {
                    (void)h.ui.table_next_row();
                    (void)h.ui.table_next_column();
                    h.ui.textf("row {}", r);
                    (void)h.ui.table_next_column();
                    h.ui.push_id(std::to_string(r));
                    if (h.ui.begin_table("inner", 2, table_default, r == 2 ? 60.0f : 0.0f)) {
                        h.ui.table_setup_column("k");
                        h.ui.table_setup_column("v");
                        (void)h.ui.table_headers_row();
                        for (int i = 0; i < 4; ++i) {
                            (void)h.ui.table_next_row();
                            (void)h.ui.table_next_column();
                            h.ui.textf("key {}", i);
                            (void)h.ui.table_next_column();
                            h.ui.textf("value {}", i);
                        }
                        h.ui.end_table();
                    } else {
                        inner_ok = false;
                    }
                    h.ui.pop_id();
                }
                h.ui.end_table();
            } else {
                inner_ok = false;
            }
            after_y[frame_no % 3] = h.ui.custom_item("after", {10, 10}).bounds.min.y;
        }
    };
    for (frame_no = 0; frame_no < 3; ++frame_no) { h.frame(build); }
    CHECK(inner_ok);
    CHECK(after_y[0] > 100.0f); // the tables take room
    CHECK(near_eq(after_y[1], after_y[2], 0.5f)); // the layout has settled and is stable
    // the rows grew to fit their inner tables: the whole thing is much taller than three text rows
    CHECK(after_y[2] > 3.0f * (h.ui.font().line_height(0) + 6.0f) + 150.0f);

    // depth limit: the sixth nested table is refused, and everything unwinds cleanly
    int opened = 0;
    const auto build_deep = [&] {
        if (auto w = h.ui.window("deep", {0, 0}, {500, 0}, plain_window)) {
            int depth = 0;
            for (; depth < 8; ++depth) {
                if (!h.ui.begin_table("t", 1)) { too_deep_refused = true; break; }
                h.ui.table_setup_column("c");
                (void)h.ui.table_next_row();
                (void)h.ui.table_next_column();
                h.ui.push_id("level");
                ++opened;
            }
            for (int i = 0; i < depth; ++i) {
                h.ui.pop_id();
                h.ui.end_table();
            }
            // the next table starts fresh
            CHECK(h.ui.begin_table("fresh", 1));
            h.ui.end_table();
        }
    };
    h.frames(build_deep, 2);
    CHECK(too_deep_refused);
    CHECK(opened >= 5 && opened <= 10);
}

// more editing: mouse selection, wheel, history limits, read-only ------------------------------------------------

void test_texture_image()
{
    std::fprintf(stderr, "[textures: formats, mip maps, updates]\n");

    CHECK(texture_mip_count(256, 256, 0) == 9);
    CHECK(texture_mip_count(256, 256, 1) == 1);
    CHECK(texture_mip_count(100, 30, 0) == 7);
    CHECK(texture_mip_count(100, 30, 3) == 3);
    CHECK(texture_mip_count(4, 4, 99) == 3); // no more than fits
    CHECK(texture_mip_count(0, 4, 0) == 0);
    CHECK(texture_source_bytes(texture_format::rgba8) == 4 && texture_source_bytes(texture_format::r8) == 1 &&
          texture_source_bytes(texture_format::rgba16f) == 8);
    CHECK(texture_layout_of(texture_format::r8) == texture_layout::rgba8 && texture_layout_of(texture_format::bgra8) == texture_layout::bgra8);

    // half floats
    CHECK(float_to_half(1.0f) == 0x3c00 && float_to_half(0.5f) == 0x3800 && float_to_half(-2.0f) == 0xc000);
    CHECK(float_to_half(65504.0f) == 0x7bff && float_to_half(1.0e6f) == 0x7c00 && float_to_half(1.0e-9f) == 0);
    CHECK(near_eq(half_to_float(0x3c00), 1.0f, 0.0f) && near_eq(half_to_float(0xc000), -2.0f, 0.0f) && near_eq(half_to_float(0x0001), 5.96e-8f, 1.0e-9f));
    CHECK(std::isnan(half_to_float(float_to_half(std::nanf("")))) && std::isinf(half_to_float(0x7c00)));
    for (const f32 v : {0.0f, 0.1f, 0.333f, 0.75f, 1.0f, 3.14159f, 100.5f}) {
        CHECK(near_eq(half_to_float(float_to_half(v)), v, v * 0.001f + 1.0e-4f));
    }

    // rgba8: every level of a flat color is that color
    {
        texture_image img;
        const std::vector<u8> flat = [] { std::vector<u8> v(4 * 4 * 4); for (std::size_t i = 0; i < v.size(); i += 4) { v[i] = 200; v[i + 1] = 100; v[i + 2] = 50; v[i + 3] = 255; } return v; }();
        CHECK(img.create({4, 4, texture_format::rgba8, 0, false}, flat));
        CHECK(img.level_count() == 3 && img.width(1) == 2 && img.width(2) == 1 && img.height(2) == 1);
        CHECK(img.pitch(1) == 8);
        const auto last = img.pixels(2);
        CHECK(last.size() == 4 && last[0] == 200 && last[1] == 100 && last[2] == 50 && last[3] == 255);
        img.wipe();
        CHECK(!img.valid());
    }
    // colors are weighted by alpha: transparent texels do not darken what is opaque
    {
        texture_image img;
        std::vector<u8> px(2 * 2 * 4, 0);
        px[0] = 255; px[1] = 0; px[2] = 0; px[3] = 255;                       // one opaque red texel
        for (int i = 1; i < 4; ++i) { px[i * 4 + 2] = 255; }                    // three fully transparent blue ones
        CHECK(img.create({2, 2, texture_format::rgba8, 0, false}, px));
        const auto p = img.pixels(1);
        CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0);     // still pure red
        CHECK(p[3] == 64);                                  // a quarter covered
    }
    // r8 is opaque grey, a8 is white coverage
    {
        texture_image g, a;
        const std::vector<u8> ramp = {0, 128, 255, 64};
        CHECK(g.create({2, 2, texture_format::r8, 1, false}, ramp) && a.create({2, 2, texture_format::a8, 1, false}, ramp));
        CHECK(g.layout() == texture_layout::rgba8 && g.pixels(0).size() == 16);
        CHECK(g.pixels(0)[4] == 128 && g.pixels(0)[5] == 128 && g.pixels(0)[6] == 128 && g.pixels(0)[7] == 255);
        CHECK(a.pixels(0)[4] == 255 && a.pixels(0)[7] == 128 && a.pixels(0)[11] == 255);
    }
    // bgra8 stays bgra8; rgba16f averages in floats
    {
        texture_image b, f;
        const std::vector<u8> px = {10, 20, 30, 255};
        CHECK(b.create({1, 1, texture_format::bgra8, 1, false}, px) && b.layout() == texture_layout::bgra8 && b.pixels(0)[0] == 10);
        std::vector<u16> half(2 * 2 * 4);
        for (int i = 0; i < 4; ++i) { half[i * 4] = float_to_half(i == 0 ? 1.0f : 0.0f); half[i * 4 + 3] = float_to_half(1.0f); }
        CHECK(f.create({2, 2, texture_format::rgba16f, 0, false}, std::span<const u8>{reinterpret_cast<const u8*>(half.data()), half.size() * 2}));
        CHECK(f.level_count() == 2 && f.pixels(0).size() == 32 && f.pixels(1).size() == 8);
        const u16* top = reinterpret_cast<const u16*>(f.pixels(1).data());
        CHECK(near_eq(half_to_float(top[0]), 0.25f, 0.001f) && near_eq(half_to_float(top[3]), 1.0f, 0.001f));
    }
    // too little data / no size: refused
    {
        texture_image img;
        CHECK(!img.create({4, 4, texture_format::rgba8, 1, false}, std::vector<u8>(10)));
        CHECK(!img.create({0, 4, texture_format::rgba8, 1, false}, std::vector<u8>(64)));
        CHECK(!img.valid());
    }
    // updates: level 0 is replaced, and the levels below are rebuilt where they touch it
    {
        texture_image img;
        CHECK(img.create({8, 8, texture_format::rgba8, 0, true}, std::vector<u8>(8 * 8 * 4, 0)));
        CHECK(img.level_count() == 4);
        std::vector<texture_image::region> dirty;
        std::vector<u8> red(3 * 3 * 4);
        for (std::size_t i = 0; i < red.size(); i += 4) { red[i] = 255; red[i + 3] = 255; }
        CHECK(img.update(2, 2, 3, 3, red, dirty));
        CHECK(dirty.size() == 4 && dirty[0].level == 0 && dirty[0].x == 2 && dirty[0].w == 3);
        CHECK(dirty[1].level == 1 && dirty[1].x == 1 && dirty[1].y == 1 && dirty[1].w == 2 && dirty[1].h == 2);
        CHECK(dirty[3].w == 1 && dirty[3].h == 1);
        const auto p0 = img.pixels(0);
        CHECK(p0[(2 * 8 + 2) * 4] == 255 && p0[(1 * 8 + 1) * 4] == 0 && p0[(4 * 8 + 4) * 4] == 255 && p0[(5 * 8 + 5) * 4] == 0);
        const auto p1 = img.pixels(1); // level 1 (4x4): the texel at (1, 1) covers (2..3, 2..3): all red
        CHECK(p1[(1 * 4 + 1) * 4] == 255 && p1[(1 * 4 + 1) * 4 + 3] == 255);
        CHECK(p1[(0 * 4 + 0) * 4 + 3] == 0);
        CHECK(!img.update(6, 6, 3, 3, red, dirty)); // does not fit
        CHECK(!img.update(0, 0, 3, 3, std::span<const u8>{red.data(), 8}, dirty)); // too few bytes
        CHECK(!img.update(0, 0, 0, 3, red, dirty));
    }
    // an update of an r8 texture takes r8 bytes
    {
        texture_image img;
        CHECK(img.create({4, 4, texture_format::r8, 0, true}, std::vector<u8>(16, 0)));
        std::vector<texture_image::region> dirty;
        CHECK(img.update(1, 1, 2, 1, std::vector<u8>{9, 10}, dirty));
        CHECK(img.pixels(0)[(1 * 4 + 1) * 4] == 9 && img.pixels(0)[(1 * 4 + 2) * 4 + 3] == 255);
    }
    // odd sizes: the chain and the updates stay inside their levels
    {
        texture_image img;
        CHECK(img.create({5, 3, texture_format::rgba8, 0, true}, std::vector<u8>(5 * 3 * 4, 255)));
        CHECK(img.level_count() == 3 && img.width(1) == 2 && img.height(1) == 1 && img.width(2) == 1);
        std::vector<texture_image::region> dirty;
        CHECK(img.update(4, 2, 1, 1, std::vector<u8>{1, 2, 3, 4}, dirty));
        for (const auto& r : dirty) { CHECK(r.x + r.w <= img.width(r.level) && r.y + r.h <= img.height(r.level) && r.w > 0 && r.h > 0); }
    }
}

// text fields: triple click, styled contents, input methods ------------------------------------------------------

void test_charts()
{
    std::fprintf(stderr, "[charts: axes, zoom, pan, fills]\n");

    // the area fill is a strip: two vertices per point
    {
        font_atlas atlas = font_atlas::build().value();
        draw_list  dl;
        dl.begin({800, 600}, atlas, 1.0f);
        const std::array<vec2, 4> pts = {{{10, 50}, {40, 20}, {70, 40}, {100, 10}}};
        dl.area_fill(pts, 100.0f, color{255, 0, 0, 200}, color{255, 0, 0, 0});
        CHECK(dl.data().vertices.size() == 8 && dl.data().indices.size() == 18);
        CHECK(dl.data().vertices[0].col.a == 200 && dl.data().vertices[1].col.a == 0); // faded toward the base
        dl.area_fill(std::span<const vec2>{pts.data(), 1}, 100.0f, color{255, 0, 0, 200}, color{255, 0, 0, 0}); // one point: nothing
        CHECK(dl.data().vertices.size() == 8);
    }

    harness h;
    std::vector<f32> data(100);
    for (std::size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<f32>(i % 10); }
    const plot_series series[] = {{"v", data, color{0, 0, 0, 0}}};
    plot_options opt;
    opt.size     = {0.0f, 200.0f};
    opt.x        = {"time", "s"};
    opt.y        = {"load", "%"};
    opt.x_start  = 10.0f;
    opt.x_step   = 0.5f;
    opt.fill     = true;
    opt.zoom_pan = true;
    vec2 range{};
    bool zoomed = false;
    std::size_t vertices_flat = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            h.ui.plot("c", series, opt);
            range  = h.ui.plot_x_range("c");
            zoomed = h.ui.plot_zoomed("c");
        }
    };
    h.frames(build, 3);
    CHECK(near_eq(range.x, 10.0f, 0.01f) && near_eq(range.y, 10.0f + 99.0f * 0.5f, 0.01f)); // the whole data, in x units
    CHECK(!zoomed);
    vertices_flat = h.ui.render_data().vertices.size();
    CHECK(vertices_flat > 200); // ticks, labels, grid, fill and line

    // the wheel over the plot zooms x around the pointer
    const vec2 mid{200.0f, 112.0f};
    h.move(mid);
    h.frames(build, 3);
    const f32 full = range.y - range.x;
    h.in.wheel = 3.0f;
    h.frame(build);
    h.frames(build, 2);
    CHECK(zoomed);
    CHECK((range.y - range.x) < full * 0.8f);
    const f32 x_mid_before = range.x + (range.y - range.x) * 0.5f;
    CHECK(x_mid_before > 20.0f && x_mid_before < 40.0f);

    // dragging pans: to the left shows later data
    const f32 lo_before = range.x;
    h.down();
    h.frame(build);
    h.move({mid.x - 60.0f, mid.y});
    h.frames(build, 3);
    h.up();
    h.frames(build, 2);
    CHECK(range.x > lo_before + 1.0f);
    const f32 zoomed_span = range.y - range.x;
    CHECK(zoomed_span < full * 0.8f); // panning does not change the zoom

    // Ctrl + wheel zooms the values (and the x range stays)
    h.move(mid);
    h.frames(build, 3);
    h.in.ctrl = true;
    h.in.wheel = 2.0f;
    h.frame(build);
    h.in.ctrl = false;
    h.frames(build, 2);
    CHECK(near_eq(range.y - range.x, zoomed_span, 0.01f));

    // a double-click resets everything
    h.click(mid, build);
    h.click(mid, build);
    h.frames(build, 3);
    CHECK(!zoomed);
    CHECK(near_eq(range.x, 10.0f, 0.01f) && near_eq(range.y, 59.5f, 0.01f));

    // zooming out is bounded, zooming in as well
    h.in.wheel = -40.0f;
    h.frame(build);
    h.frames(build, 2);
    CHECK((range.y - range.x) <= full * 4.0f + 0.01f);
    h.in.wheel = 200.0f;
    h.frame(build);
    h.frames(build, 2);
    CHECK((range.y - range.x) >= opt.x_step * 2.0f - 0.01f);
    h.ui.plot_reset_view("c"); // (outside the window: another id scope, so this does nothing)

    // a lot of samples: bucketed, and a histogram, without trouble
    std::vector<f32> many(50000);
    for (std::size_t i = 0; i < many.size(); ++i) { many[i] = std::sin(static_cast<f32>(i) * 0.01f); }
    const plot_series big[] = {{"many", many, color{0, 0, 0, 0}}};
    plot_options bigopt = opt;
    bigopt.x_start = 0.0f;
    bigopt.x_step = 1.0f;
    const auto build_big = [&] {
        if (auto w = h.ui.window("big", {0, 0}, {400, 0}, plain_window)) {
            h.ui.plot("line", big, bigopt);
            plot_options hist = bigopt;
            hist.kind = plot_kind::histogram;
            h.ui.plot("bars", big, hist);
        }
    };
    h.frames(build_big, 3);
    CHECK(h.ui.render_data().vertices.size() < 40000u);

    // empty and single-sample data are fine
    const std::vector<f32> none;
    const std::vector<f32> one{3.0f};
    const plot_series s_none[] = {{"n", none, color{0, 0, 0, 0}}};
    const plot_series s_one[]  = {{"o", one, color{0, 0, 0, 0}}};
    h.frames([&] {
        if (auto w = h.ui.window("edge", {0, 0}, {400, 0}, plain_window)) {
            h.ui.plot("e0", s_none, opt);
            h.ui.plot("e1", s_one, opt);
        }
    }, 3);
}

// acrylic: blurred-frame saturation / brightness, glass popups ---------------------------------------------------

void test_plots()
{
    std::fprintf(stderr, "[plots]\n");
    harness h;
    std::vector<f32> data(200);
    for (std::size_t i = 0; i < data.size(); ++i) { data[i] = std::sin(static_cast<f32>(i) * 0.1f); }
    std::vector<f32> huge(20000);
    for (std::size_t i = 0; i < huge.size(); ++i) { huge[i] = std::sin(static_cast<f32>(i) * 0.01f); }
    u32 offset = 0;
    int which = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            switch (which) {
            case 0: h.ui.plot_lines("##p", data, {0, 80}, "overlay", plot_auto, plot_auto, offset); break;
            case 1: h.ui.plot_histogram("##p", data, {0, 80}); break;
            case 2: h.ui.plot_lines("##p", huge, {0, 80}); break;
            default: {
                const std::array<plot_series, 2> s = {{{"a", data, color{0, 0, 0, 0}}, {"b", data, color::from_hex(0xff0000ffu)}}};
                h.ui.plot("##p", s, {0, 100});
                h.ui.sparkline(data, {80, 20});
                break;
            }
            }
        }
    };
    h.frames(build, 3);
    const std::size_t base_vertices = h.ui.render_data().vertices.size();
    CHECK(base_vertices > 200);

    // hovering shows the cursor and a tooltip: more geometry, in an overlay
    h.move({200.0f, 40.0f});
    h.frames(build, 3);
    CHECK(h.ui.render_data().vertices.size() > base_vertices);

    // ring offsets and other plot kinds must not crash; huge series decimate to about a vertex per pixel
    offset = 57;
    h.frames(build, 2);
    which = 1;
    h.frames(build, 2);
    which = 2;
    h.move({-500.0f, -500.0f});
    h.frames(build, 3);
    CHECK(h.ui.render_data().vertices.size() < 20000u); // 20000 samples were reduced
    which = 3;
    h.frames(build, 3);
    CHECK(!h.ui.render_data().commands.empty());
    which = 0;
    data.clear(); // an empty series draws the frame only
    h.frames(build, 2);
    CHECK(true);
}

void test_number_widgets()
{
    std::fprintf(stderr, "[drag sliders and number inputs]\n");
    harness h;
    f32 v = 1.0f;
    int n = 10;
    f32 vec[3] = {0.0f, 0.5f, 1.0f};
    f32 typed = 2.0f;
    int typed_int = 5;
    int which = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            switch (which) {
            case 0: (void)h.ui.drag_float("##d", v, 0.1f, 0.0f, 100.0f, 2); break;
            case 1: (void)h.ui.drag_int("##i", n, 0.25f, 0, 100); break;
            case 2: (void)h.ui.drag_float3("##v", vec, 0.01f, 0.0f, 1.0f, 2); break;
            default:
                (void)h.ui.input_float("##f", typed, 0.5f, 2);
                (void)h.ui.input_int("##n", typed_int, 2);
                break;
            }
        }
    };
    const f32 fh = h.ui.frame_height();
    const vec2 mid{200.0f, 12.0f + fh * 0.5f};

    // dragging: 40 px * 0.1 per pixel
    h.move(mid);
    h.frames(build, 3);
    h.down();
    h.frame(build);
    for (int i = 1; i <= 4; ++i) { h.move({mid.x + 10.0f * i, mid.y}); h.frame(build); }
    h.up();
    h.frames(build, 2);
    const f32 per_px = 100.0f / 376.0f; // a ranged field spans its range over its width
    CHECK(near_eq(v, 1.0f + 40.0f * per_px, 0.05f));

    // shift = fine (a tenth), and the range clamps
    h.in.shift = true;
    h.move(mid);
    h.frame(build);
    h.down();
    h.frame(build);
    h.move({mid.x + 100.0f, mid.y});
    h.frame(build);
    h.up();
    h.in.shift = false;
    h.frames(build, 2);
    CHECK(near_eq(v, 1.0f + 40.0f * per_px + 10.0f * per_px, 0.1f));
    h.move(mid);
    h.frame(build);
    h.down();
    h.frame(build);
    h.move({mid.x + 3000.0f, mid.y});
    h.frame(build);
    h.up();
    h.frames(build, 2);
    CHECK(near_eq(v, 100.0f, 0.01f)); // clamped to hi

    // a click without dragging starts typing; Enter applies
    h.move(mid);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.up();
    h.frames(build, 3);
    CHECK(h.ui.want_text_input());
    h.key(key::a, true);
    h.frame(build);
    h.type("42.5");
    h.frame(build);
    h.key(key::enter);
    h.frame(build);
    h.frames(build, 2);
    CHECK(near_eq(v, 42.5f, 0.001f));
    CHECK(!h.ui.want_text_input());

    // Esc cancels the entry
    h.move(mid);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.up();
    h.frames(build, 3);
    h.key(key::a, true);
    h.frame(build);
    h.type("7");
    h.frame(build);
    h.key(key::escape);
    h.frame(build);
    h.frames(build, 2);
    CHECK(near_eq(v, 42.5f, 0.001f));

    // an integer drag moves in whole steps
    which = 1;
    h.frames(build, 2);
    h.move(mid);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    for (int i = 1; i <= 5; ++i) { h.move({mid.x + 8.0f * i, mid.y}); h.frame(build); } // 40 px * 0.25 = 10
    h.up();
    h.frames(build, 2);
    CHECK(n == 20);

    // vectors: each component is its own field
    which = 2;
    h.frames(build, 2);
    const f32 comp_w = (376.0f - 8.0f) / 3.0f;
    const vec2 second{12.0f + comp_w + 4.0f + comp_w * 0.5f, mid.y};
    h.move(second);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.move({second.x - 20.0f, second.y});
    h.frame(build);
    h.up();
    h.frames(build, 2);
    CHECK(near_eq(vec[0], 0.0f) && near_eq(vec[1], 0.5f - 20.0f / comp_w, 0.02f) && near_eq(vec[2], 1.0f));

    // number fields: partial text leaves the value, a parsed number sets it, the buttons step it
    which = 3;
    h.frames(build, 2);
    h.click({100.0f, mid.y}, build);
    h.key(key::a, true);
    h.frame(build);
    h.type("-");
    h.frame(build);
    CHECK(near_eq(typed, 2.0f)); // "-" does not parse
    h.type("3.25");
    h.frame(build);
    CHECK(near_eq(typed, -3.25f, 0.001f));
    h.click({700.0f, 500.0f}, build);
    const f32 bw = fh;
    const vec2 plus{12.0f + 376.0f - bw * 0.5f, mid.y};
    h.click(plus, build);
    CHECK(near_eq(typed, -2.75f, 0.001f)); // + step 0.5
    const vec2 int_field{100.0f, 12.0f + fh + 7.0f + fh * 0.5f};
    h.click(int_field, build);
    h.key(key::a, true);
    h.frame(build);
    h.type("12");
    h.frame(build);
    CHECK(typed_int == 12);
}

void test_tabs()
{
    std::fprintf(stderr, "[tabs: closable, reorderable, add button, overflow]\n");

    // apply_tab_events keeps `selected` on the tab it was on
    {
        std::vector<std::string> v{"a", "b", "c", "d"};
        int sel = 1;
        tab_events ev;
        ev.moved_from = 1; ev.moved_to = 3;
        apply_tab_events(ev, v, sel);
        CHECK(v == (std::vector<std::string>{"a", "c", "d", "b"}) && sel == 3);
        sel = 2; ev.moved_from = 0; ev.moved_to = 3;
        apply_tab_events(ev, v, sel);
        CHECK(v == (std::vector<std::string>{"c", "d", "b", "a"}) && sel == 1);
        sel = 1; ev = {}; ev.closed = 0;
        apply_tab_events(ev, v, sel);
        CHECK(v.size() == 3 && sel == 0);
        sel = 2; ev.closed = 2;
        apply_tab_events(ev, v, sel);
        CHECK(v.size() == 2 && sel == 1); // the last tab closed: the one before it
    }

    harness h;
    std::vector<std::string> names{"Alpha", "Beta", "Gamma", "Delta"};
    int sel = 0;
    tab_events ev;
    tab_bar_flags flags = tab_bar_flags::closable | tab_bar_flags::reorderable | tab_bar_flags::add_button;
    int adds = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {100, 100}, {500, 0}, plain_window)) {
            std::vector<tab_desc> d;
            for (const std::string& n : names) { d.emplace_back(std::string_view{n}); }
            ev = h.ui.tab_bar("tabs", d.data(), d.size(), sel, flags);
            if (ev.add) { ++adds; }
        }
        apply_tab_events(ev, names, sel);
    };
    const auto tab_w = [&](std::size_t i, bool closable) { return h.ui.font().measure(0, names[i]).x + 26.0f + (closable ? 20.0f : 0.0f); };
    const auto body_center = [&](std::size_t i) {
        f32 x = 112.0f;
        for (std::size_t j = 0; j < i; ++j) { x += tab_w(j, true) + 2.0f; }
        return vec2{x + (tab_w(i, true) - 20.0f) * 0.5f, 112.0f + h.ui.frame_height() * 0.5f};
    };
    const auto close_center = [&](std::size_t i) {
        f32 x = 112.0f;
        for (std::size_t j = 0; j <= i; ++j) { x += tab_w(j, true) + 2.0f; }
        return vec2{x - 2.0f - 11.0f, 112.0f + h.ui.frame_height() * 0.5f};
    };

    h.frames(build, 3);
    h.click(body_center(2), build);
    CHECK(sel == 2 && names.size() == 4);

    // the "+" comes after the last tab
    {
        f32 x = 112.0f;
        for (std::size_t j = 0; j < names.size(); ++j) { x += tab_w(j, true) + 2.0f; }
        h.click({x + 2.0f + 12.0f, 112.0f + h.ui.frame_height() * 0.5f}, build);
        CHECK(adds == 1);
    }

    // the x closes the tab once events are applied; the selection stays on Gamma
    h.click(close_center(1), build); // Beta
    CHECK((names == std::vector<std::string>{"Alpha", "Gamma", "Delta"}) && names[static_cast<std::size_t>(sel)] == "Gamma");

    // dragging a tab past its neighbours moves it and it stays selected
    {
        const vec2 from = body_center(0);
        h.move(from);
        h.frames(build, 2);
        h.down();
        h.frame(build);
        for (int i = 1; i <= 12; ++i) {
            h.move({from.x + 18.0f * static_cast<f32>(i), from.y});
            h.frame(build);
        }
        h.up();
        h.frames(build, 2);
        CHECK(names.size() == 3 && names[0] != "Alpha" && names[static_cast<std::size_t>(sel)] == "Alpha");
        CHECK(names.back() == "Alpha"); // 216 px to the right passes both other tabs
    }

    // many tabs scroll; the list button at the right end opens a menu of all of them
    {
        names.clear();
        for (int i = 0; i < 14; ++i) { names.push_back("Document " + std::to_string(i)); }
        flags = tab_bar_flags::closable;
        sel = 0;
        h.frames(build, 4);
        const f32 list_x = 112.0f + 476.0f - 13.0f;
        h.click({list_x, 112.0f + h.ui.frame_height() * 0.5f}, build);
        h.frames(build, 2);
        CHECK(h.ui.any_popup_open());
        CHECK(h.ui.want_capture_mouse());
        h.click({30.0f, 500.0f}, build); // outside the list
        CHECK(!h.ui.any_popup_open());

        // the list is at most nine rows tall and scrolls; a row picks its tab
        h.click({list_x, 112.0f + h.ui.frame_height() * 0.5f}, build);
        h.frames(build, 2);
        {
            const f32 lh    = h.ui.font().line_height(0);
            const f32 pitch = lh + 8.0f + 7.0f;
            const f32 top   = 111.0f + h.ui.frame_height() + 4.0f + 12.0f; // under the list button, plus the popup padding
            h.click({650.0f, top + 3.0f * pitch + (lh + 8.0f) * 0.5f}, build); // about the fourth row (the popup sits at 556 .. 796)
            h.frames(build, 2);
            CHECK(sel == 3 && !h.ui.any_popup_open());
        }

        // selecting a far tab from outside scrolls it into view and nothing breaks with the wheel
        sel = 13;
        h.frames(build, 30);
        h.move({300.0f, 125.0f});
        h.in.wheel = 1.0f;
        h.frames(build, 20);
        h.in.wheel = 0.0f;
        CHECK(sel == 13 && names.size() == 14);

        // and the tabs that fit are not scrolled at all
        names.resize(3);
        sel = 0;
        h.frames(build, 30);
        h.click(body_center(1), build);
        CHECK(sel == 1);
    }
}

void test_datetime_pickers()
{
    std::fprintf(stderr, "[dates, times, pickers]\n");

    // calendar arithmetic
    CHECK(is_leap_year(2024) && !is_leap_year(1900) && is_leap_year(2000) && !is_leap_year(2023));
    CHECK(days_in_month(2024, 2) == 29 && days_in_month(2023, 2) == 28 && days_in_month(2026, 9) == 30 && days_in_month(2026, 12) == 31);
    CHECK(days_from_civil({1970, 1, 1}) == 0 && days_from_civil({2000, 3, 1}) == 11017 && days_from_civil({1969, 12, 31}) == -1);
    CHECK((civil_from_days(days_from_civil({2026, 9, 25})) == date{2026, 9, 25}));
    CHECK(weekday({1970, 1, 1}) == 3);  // a Thursday
    CHECK(weekday({2026, 9, 25}) == 4); // a Friday
    CHECK(weekday({2000, 1, 1}) == 5 && weekday({2024, 2, 29}) == 3);
    CHECK((add_days({2026, 12, 31}, 1) == date{2027, 1, 1}) && (add_days({2024, 3, 1}, -1) == date{2024, 2, 29}));
    CHECK((add_months({2026, 1, 31}, 1) == date{2026, 2, 28}) && (add_months({2026, 11, 15}, 3) == date{2027, 2, 15}));
    CHECK((add_months({2026, 1, 15}, -1) == date{2025, 12, 15}) && (add_months({2026, 3, 31}, -12) == date{2025, 3, 31}));
    CHECK(is_valid(date{2024, 2, 29}) && !is_valid(date{2023, 2, 29}) && !is_valid(date{2026, 13, 1}) && !is_valid(date{2026, 4, 31}));
    CHECK((clamp_date({2026, 2, 40}) == date{2026, 2, 28}) && (clamp_date({2026, 0, 0}) == date{2026, 1, 1}));
    CHECK(date{2026, 9, 25} < date{2026, 10, 1} && date{2027, 1, 1} > date{2026, 12, 31} && time_of_day{9, 5, 0} < time_of_day{9, 6, 0});

    // text
    CHECK(to_string(date{2026, 9, 5}) == "2026-09-05" && to_string(time_of_day{7, 3, 9}) == "07:03" && to_string(time_of_day{7, 3, 9}, true) == "07:03:09");
    date d;
    time_of_day t;
    CHECK(parse_date("2026-09-25", d) && d == date{2026, 9, 25});
    CHECK(parse_date(" 2024/2/29 ", d) && d == date{2024, 2, 29});
    CHECK(parse_date("2026.12.01", d) && d == date{2026, 12, 1});
    CHECK(!parse_date("2023-02-29", d) && !parse_date("2026-13-01", d) && !parse_date("2026-09", d) && !parse_date("2026-09-25-", d));
    CHECK(!parse_date("", d) && !parse_date("abc", d) && !parse_date("2026-9-x", d) && d == date{2026, 12, 1}); // untouched on failure
    CHECK(parse_time("13:45", t) && t == time_of_day{13, 45, 0} && parse_time("07:03:09", t) && t == time_of_day{7, 3, 9});
    CHECK(!parse_time("24:00", t) && !parse_time("12:60", t) && !parse_time("12", t) && !parse_time("12:", t) && t == time_of_day{7, 3, 9});
    CHECK(month_name(1) == "January" && month_name(12) == "December" && weekday_short(0) == "Mo" && weekday_short(6) == "Su");
    CHECK(is_valid(today()) && is_valid(now()));

    // the pickers: open the popup, choose, close
    harness h;
    date        day{2026, 9, 25};
    time_of_day clock{13, 45, 0};
    bool        day_changed = false, time_changed = false;
    rect        day_box, clock_box;
    bool        boxes_known = false; // (while a popup is open the last item is one of its buttons)
    const auto build = [&] {
        if (auto w = h.ui.window("p", {100, 100}, {320, 0}, plain_window)) {
            day_changed  = h.ui.date_picker("day", day) || day_changed;
            if (!boxes_known) { day_box = h.ui.item_rect(); }
            time_changed = h.ui.time_picker("time", clock) || time_changed;
            if (!boxes_known) { clock_box = h.ui.item_rect(); }
        }
    };
    h.frames(build, 3);
    boxes_known = true;
    CHECK(day_box.width() > 100.0f && clock_box.min.y > day_box.max.y);

    // the calendar: September 2026 starts on a Tuesday, so the grid begins on Monday 31 August
    h.click(day_box.center(), build);
    h.frames(build, 3);
    CHECK(h.ui.any_popup_open());
    {
        const f32 fh    = h.ui.frame_height();
        const f32 pad   = 12.0f;
        const f32 head  = fh - 4.0f;
        const f32 lh    = h.ui.font().line_height(0);
        const f32 ch    = fh - 6.0f;
        const f32 gx    = day_box.min.x + pad;                       // the grid's left edge
        const f32 w     = std::max(day_box.width(), 260.0f) - 2.0f * pad;
        const f32 cw    = w / 7.0f;
        const f32 top   = day_box.max.y + 4.0f + pad;                // the header row
        const f32 grid0 = top + head + 2.0f + (lh + 4.0f) + 2.0f;    // the first week
        // 1 September is a Tuesday (column 1) of row 0: the 9th is the second Wednesday (row 1, column 2)
        h.click({gx + cw * 2.5f, grid0 + (ch + 2.0f) * 1.5f}, build);
        h.frames(build, 2);
        CHECK(day_changed && day == date{2026, 9, 9});
        CHECK(!h.ui.any_popup_open());
    }

    // next month with the arrow, then a day: the popup shows the month of the value when it opens
    day_changed = false;
    h.click(day_box.center(), build);
    h.frames(build, 3);
    {
        const f32 fh   = h.ui.frame_height();
        const f32 pad  = 12.0f;
        const f32 head = fh - 4.0f;
        const f32 w    = std::max(day_box.width(), 260.0f) - 2.0f * pad;
        const f32 top  = day_box.max.y + 4.0f + pad;
        const vec2 next{day_box.min.x + pad + w - head * 1.5f, top + head * 0.5f}; // the single right arrow
        h.click(next, build);
        h.click(next, build);
        CHECK(!day_changed && h.ui.any_popup_open()); // moving through months changes nothing
        // October, then November 2026: 1 November is a Sunday, the last column of row 0
        const f32 lh    = h.ui.font().line_height(0);
        const f32 ch    = fh - 6.0f;
        const f32 cw    = w / 7.0f;
        const f32 grid0 = top + head + 2.0f + (lh + 4.0f) + 2.0f;
        h.click({day_box.min.x + pad + cw * 6.5f, grid0 + ch * 0.5f}, build);
        h.frames(build, 2);
        CHECK(day_changed && day == date{2026, 11, 1});
    }

    // the time grid: an hour, a minute and the fine step
    h.click(clock_box.center(), build);
    h.frames(build, 3);
    CHECK(h.ui.any_popup_open());
    {
        const f32 fh  = h.ui.frame_height();
        const f32 pad = 12.0f;
        const f32 ch  = fh - 6.0f;
        const f32 lh  = h.ui.font().line_height(0);
        const f32 w   = std::max(clock_box.width(), 240.0f) - 2.0f * pad;
        const f32 cw  = w / 6.0f;
        const f32 x0  = clock_box.min.x + pad;
        const f32 top = clock_box.max.y + 4.0f + pad;       // the caption "hour"
        const f32 row0 = top + lh + 2.0f;                    // the first row of hours (the pickers space their rows by 2 px)
        // hour 8 is the third column of the second row
        h.click({x0 + cw * 2.5f, row0 + (ch + 2.0f) * 1.5f}, build);
        CHECK(time_changed && clock.hour == 8 && clock.minute == 45);
        time_changed = false;
        // minutes: the grid comes after the four rows of hours, a gap and the caption
        const f32 min0 = row0 + 4.0f * ch + 3.0f * 2.0f + 2.0f + 4.0f + 2.0f + lh + 2.0f;
        h.click({x0 + cw * 3.5f, min0 + ch * 0.5f}, build); // 15
        CHECK(time_changed && clock.minute == 15);
        // the + button, at the right end of the row under the two rows of minutes
        h.click({x0 + w - 17.0f, min0 + 2.0f * (ch + 2.0f) + ch * 0.5f}, build);
        CHECK(clock.minute == 16);
        h.click({x0 + 17.0f, min0 + 2.0f * (ch + 2.0f) + ch * 0.5f}, build);
        h.click({x0 + 17.0f, min0 + 2.0f * (ch + 2.0f) + ch * 0.5f}, build);
        CHECK(clock.minute == 14);
    }
    h.key(key::escape);
    h.frames(build, 3);
    CHECK(!h.ui.any_popup_open());
    CHECK(clock.hour == 8 && day == date{2026, 11, 1});

    // an invalid value is brought back to a valid one
    date bad{2026, 2, 40};
    time_of_day bad_t{30, 99, 99};
    const auto build_bad = [&] {
        if (auto w = h.ui.window("p", {100, 100}, {320, 0}, plain_window)) {
            (void)h.ui.date_picker("day", bad);
            (void)h.ui.time_picker("time", bad_t, true);
            (void)h.ui.datetime_picker("both", bad, bad_t);
        }
    };
    h.frames(build_bad, 3);
    CHECK((bad == date{2026, 2, 28}) && (bad_t == time_of_day{23, 59, 59}));
}

void test_lists_and_tables()
{
    std::fprintf(stderr, "[list clipper, table extras, tree tables]\n");

    // a long list in a fixed-height window: only visible rows are submitted, the scroll range covers the whole list
    {
        harness h;
        int first = 1 << 30, last = -1, submitted = 0;
        const auto build = [&] {
            first = 1 << 30; last = -1; submitted = 0;
            if (auto w = h.ui.window("l", {50, 50}, {300, 300}, plain_window)) {
                list_clipper clip(h.ui, 1000, h.ui.frame_height());
                while (clip.step()) {
                    for (int i = clip.begin(); i < clip.end(); ++i) {
                        ++submitted;
                        first = std::min(first, i);
                        last  = std::max(last, i);
                        (void)h.ui.custom_item("r" + std::to_string(i), {100.0f, h.ui.frame_height()});
                    }
                }
            }
        };
        h.frames(build, 4);
        CHECK(first == 0 && last > 3 && last < 16 && submitted < 20);
        h.move({150.0f, 150.0f});
        h.frames(build, 2);
        for (int i = 0; i < 200; ++i) { h.in.wheel = -1.0f; h.frame(build); } // 48 px each
        h.frames(build, 2);
        CHECK(first > 200 && first < 400 && submitted < 20); // scrolled far into the list, still a handful of rows
        for (int i = 0; i < 1000; ++i) { h.in.wheel = -1.0f; h.frame(build); }
        h.frames(build, 2);
        CHECK(last == 999 && first > 970); // the end of the list is reachable: the spacers add up
        for (int i = 0; i < 1200; ++i) { h.in.wheel = 1.0f; h.frame(build); }
        h.frames(build, 2);
        CHECK(first == 0);

        // an empty list and a short one
        int rows_drawn = 0;
        const auto build_short = [&] {
            rows_drawn = 0;
            if (auto w = h.ui.window("s", {400, 50}, {300, 300}, plain_window)) {
                for (const std::size_t n : {std::size_t{0}, std::size_t{3}}) {
                    list_clipper clip(h.ui, n, 0.0f);
                    while (clip.step()) { for (int i = clip.begin(); i < clip.end(); ++i) { ++rows_drawn; h.ui.text("x"); } }
                }
            }
        };
        h.frames(build_short, 3);
        CHECK(rows_drawn == 3);
    }

    // a big table with a scrolling body
    {
        harness h;
        int submitted = 0, last = -1;
        const auto build = [&] {
            submitted = 0; last = -1;
            if (auto w = h.ui.window("t", {50, 50}, {400, 320}, plain_window)) {
                if (h.ui.begin_table("big", 2, table_default, 240.0f)) {
                    h.ui.table_setup_column("row");
                    h.ui.table_setup_column("value", 100.0f);
                    (void)h.ui.table_headers_row();
                    list_clipper clip(h.ui, 5000);
                    while (clip.step()) {
                        for (int i = clip.begin(); i < clip.end(); ++i) {
                            if (h.ui.table_next_row()) {
                                ++submitted;
                                last = i;
                                h.ui.table_next_column(); h.ui.textf("row {}", i);
                                h.ui.table_next_column(); h.ui.textf("{}", i * 3);
                            }
                        }
                    }
                    h.ui.end_table();
                }
            }
        };
        h.frames(build, 4);
        CHECK(submitted > 3 && submitted < 30);
        h.move({150.0f, 200.0f});
        for (int i = 0; i < 3000; ++i) { h.in.wheel = -1.0f; h.frame(build); if (last == 4999) { break; } }
        h.frames(build, 2);
        CHECK(last == 4999 && submitted < 30);
    }

    // table extras: columns hidden by default, moved by dragging the header, saved and loaded
    {
        harness h;
        bool shown[3]{};
        std::string layout;
        const auto build = [&] {
            if (auto w = h.ui.window("x", {100, 100}, {480, 0}, plain_window)) {
                if (h.ui.begin_table("cols", 3, table_default | table_flags::hideable | table_flags::reorderable)) {
                    h.ui.table_setup_column("A", 0.0f, 1.0f, table_column_flags::no_hide);
                    h.ui.table_setup_column("B");
                    h.ui.table_setup_column("C", 0.0f, 1.0f, table_column_flags::default_hidden);
                    (void)h.ui.table_headers_row();
                    if (h.ui.table_next_row()) {
                        shown[0] = h.ui.table_next_column(); h.ui.text("a");
                        shown[1] = h.ui.table_next_column(); h.ui.text("b");
                        shown[2] = h.ui.table_next_column(); h.ui.text("c");
                    }
                    h.ui.end_table();
                }
                layout = h.ui.table_save_layout("cols");
            }
        };
        h.frames(build, 3);
        CHECK(shown[0] && shown[1] && !shown[2]); // C starts hidden: its cell says so
        CHECK(layout == "order=0,1,2;hidden=2;widths=0.3333,0.3333,0.3333");

        // drag A to the right, past the middle of B
        const f32 hy = 112.0f + 4.0f;
        const f32 w  = 480.0f - 24.0f;
        const vec2 from{112.0f + w * 0.25f, hy};
        h.move(from);
        h.frames(build, 2);
        h.down();
        h.frame(build);
        for (int i = 1; i <= 10; ++i) { h.move({from.x + w * 0.07f * static_cast<f32>(i), hy}); h.frame(build); }
        h.up();
        h.frames(build, 2);
        CHECK(layout.rfind("order=1,0,2", 0) == 0);
        CHECK(shown[0] && shown[1] && !shown[2]); // the cells still follow the declared columns

        // a saved layout can be loaded into another table before it is ever drawn
        harness g;
        bool g_shown[3]{};
        std::string g_layout;
        bool loaded = false;
        const auto build_g = [&] {
            if (auto w = g.ui.window("x", {100, 100}, {480, 0}, plain_window)) {
                if (!loaded) { g.ui.table_load_layout("cols", "order=2,0,1;hidden=1;widths=0.5,0.25,0.25"); loaded = true; }
                if (g.ui.begin_table("cols", 3, table_default | table_flags::hideable | table_flags::reorderable)) {
                    g.ui.table_setup_column("A", 0.0f, 1.0f, table_column_flags::no_hide);
                    g.ui.table_setup_column("B");
                    g.ui.table_setup_column("C", 0.0f, 1.0f, table_column_flags::default_hidden);
                    (void)g.ui.table_headers_row();
                    if (g.ui.table_next_row()) {
                        g_shown[0] = g.ui.table_next_column(); g.ui.text("a");
                        g_shown[1] = g.ui.table_next_column(); g.ui.text("b");
                        g_shown[2] = g.ui.table_next_column(); g.ui.text("c");
                    }
                    g.ui.end_table();
                }
                g_layout = g.ui.table_save_layout("cols");
            }
        };
        g.frames(build_g, 3);
        CHECK(g_layout == "order=2,0,1;hidden=1;widths=0.5000,0.2500,0.2500");
        CHECK(g_shown[0] && !g_shown[1] && g_shown[2]); // the file overrides the defaults (C shown, B hidden)

        // a layout that does not fit is ignored
        g.frames(build_g, 1);
        std::string before = g_layout;
        g.ui.begin_frame(g.in);
        g.ui.end_frame();
        bool again = false;
        const auto build_bad = [&] {
            if (auto w = g.ui.window("x", {100, 100}, {480, 0}, plain_window)) {
                if (!again) { g.ui.table_load_layout("cols", "order=0,0,1;hidden=7;widths=a,b"); again = true; }
                if (g.ui.begin_table("cols", 3, table_default)) {
                    g.ui.table_setup_column("A"); g.ui.table_setup_column("B"); g.ui.table_setup_column("C");
                    g.ui.end_table();
                }
                g_layout = g.ui.table_save_layout("cols");
            }
        };
        g.frames(build_bad, 3);
        CHECK(g_layout == before);

        // right-clicking the header opens the column menu
        h.move({112.0f + w * 0.5f, hy});
        h.frames(build, 2);
        h.in.mouse_down[1] = true;
        h.frame(build);
        h.in.mouse_down[1] = false;
        h.frames(build, 2);
        CHECK(h.ui.menu_open());
    }

    // a tree table: the children are rows, shown while the node is open
    {
        harness h;
        int rows = 0;
        bool pressed_leaf = false;
        const auto build = [&] {
            rows = 0;
            if (auto w = h.ui.window("tt", {100, 100}, {480, 0}, plain_window)) {
                if (h.ui.begin_table("tree", 2, table_default)) {
                    h.ui.table_setup_column("name");
                    h.ui.table_setup_column("kind", 100.0f);
                    h.ui.table_next_row(); ++rows;
                    h.ui.table_next_column();
                    const bool open = h.ui.table_tree_node("root");
                    h.ui.table_next_column(); h.ui.text("folder");
                    if (open) {
                        h.ui.table_next_row(); ++rows;
                        h.ui.table_next_column();
                        if (h.ui.table_tree_leaf("readme")) { pressed_leaf = true; }
                        h.ui.table_next_column(); h.ui.text("file");
                        h.ui.table_next_row(); ++rows;
                        h.ui.table_next_column();
                        const bool sub_open = h.ui.table_tree_node("src");
                        h.ui.table_next_column(); h.ui.text("folder");
                        if (sub_open) {
                            h.ui.table_next_row(); ++rows;
                            h.ui.table_next_column();
                            (void)h.ui.table_tree_leaf("main");
                            h.ui.table_next_column(); h.ui.text("file");
                            h.ui.table_tree_pop();
                        }
                        h.ui.table_tree_pop();
                    }
                    h.ui.end_table();
                }
            }
        };
        h.frames(build, 3);
        CHECK(rows == 1);
        const f32 row_h = h.ui.font().line_height(0) + 6.0f;
        const f32 x0 = 112.0f + 8.0f; // the cell padding
        h.click({x0 + 8.0f, 112.0f + row_h * 0.5f}, build); // the arrow of "root"
        h.frames(build, 2);
        CHECK(rows == 3);
        h.click({x0 + 18.0f + 8.0f, 112.0f + row_h * 2.5f}, build); // the arrow of "src", one level in
        h.frames(build, 2);
        CHECK(rows == 4);
        h.click({x0 + 40.0f, 112.0f + row_h * 1.5f}, build); // the leaf "readme"
        CHECK(pressed_leaf);
        h.click({x0 + 8.0f, 112.0f + row_h * 0.5f}, build); // closing root hides everything below it
        h.frames(build, 2);
        CHECK(rows == 1);
    }
}

void test_item_list()
{
    std::fprintf(stderr, "[item_list: braced, containers, projections, combo over a vector]\n");
    const auto third = [](const item_list& l) { return l.size() == 3 && l[2] == "ccc"; }; // (a braced list lives for the call)
    CHECK(third({"a", "bb", "ccc"}));
    const std::vector<std::string> owned{"red", "green"};
    CHECK(item_list{owned}.size() == 2 && item_list{owned}[1] == "green");
    const std::array<std::string_view, 2> views{"x", "y"};
    CHECK(item_list{views}[0] == "x");
    const char* const raw[] = {"one", "two", "three"};
    CHECK(item_list{raw}.size() == 3 && item_list{raw}[2] == "three");
    struct unit { int factor; std::string name; };
    const std::vector<unit> units{{1, "m"}, {1000, "km"}};
    CHECK(item_list(units, &unit::name)[1] == "km");
    CHECK(item_list(units, [](const unit& u) -> std::string_view { return u.name; })[0] == "m");
    const std::string_view* none = nullptr;
    CHECK(item_list(none, 5).empty());

    harness h;
    int pick = 5; // out of range: clamped to the last entry
    const auto build = [&] {
        if (auto w = h.ui.window("list", {20, 20}, {300, 0}, plain_window)) { (void)h.ui.combo("unit", pick, {units, &unit::name}); }
    };
    h.frames(build, 2);
    CHECK(pick == 1);
}

void test_ui_strings()
{
    std::fprintf(stderr, "[ui_strings: the words widgets draw themselves come from the table]\n");
    harness h;
    key k = key::none;
    const auto build = [&] {
        if (auto w = h.ui.window("words", {20, 20}, {300, 0}, plain_window)) { (void)h.ui.hotkey("shortcut", k); }
    };
    h.frames(build, 2);
    const u32 english = h.ui.stats().vertices;
    ui_strings de = h.ui.strings();
    de.unbound = "Keine Taste belegt"; // longer than "None": more glyphs
    h.ui.set_strings(de);
    h.frames(build, 2);
    CHECK(h.ui.strings().unbound == "Keine Taste belegt" && h.ui.strings().today == "Today");
    CHECK(h.ui.stats().vertices > english);
}

void test_scrollbar_drag()
{
    std::fprintf(stderr, "[scrollbar thumbs follow the pointer]\n");

    struct thumb_shape { bool found{}; f32 x{}, y{}, h{}; };
    // the scrollbar thumb is the one 5 px wide shape
    const auto find_thumb = [](const harness& h) {
        thumb_shape t;
        for (const shape_record& s : h.ui.render_data().shapes) {
            if (std::abs(s.half_size.x * 2.0f - 5.0f) < 0.01f && s.half_size.y > 5.0f) {
                t = {true, s.center.x, s.center.y - s.half_size.y, s.half_size.y * 2.0f};
            }
        }
        return t;
    };

    // drag down and back: the thumb tracks the pointer exactly regardless of list size and returns to its start.
    // `build` draws one scrolling area with a thumb
    const auto drag_test = [&](harness& h, const auto& build, const char* what, bool expect_min_thumb) {
        h.frames(build, 4);
        h.move({100.0f, 100.0f}); // (over the content: the wheel is not used here)
        thumb_shape t = find_thumb(h);
        CHECK(t.found);
        const f32 start_y = t.y, height = t.h;
        if (expect_min_thumb) { CHECK(near_eq(height, 20.0f, 0.01f)); } // a huge list: the thumb is at its minimum size
        f32 my = t.y + t.h * 0.5f;
        h.move({t.x, my});
        h.frames(build, 2);
        h.down();
        h.frame(build);
        bool follows = true, keeps_size = true;
        for (int i = 1; i <= 8; ++i) {
            my += 6.0f;
            h.move({t.x, my});
            h.frame(build);
            const thumb_shape now = find_thumb(h);
            follows    = follows && near_eq(now.y - start_y, 6.0f * static_cast<f32>(i), 0.05f);
            keeps_size = keeps_size && near_eq(now.h, height, 0.01f);
        }
        CHECK(follows && keeps_size);
        for (int i = 1; i <= 8; ++i) {
            my -= 6.0f;
            h.move({t.x, my});
            h.frame(build);
        }
        h.up();
        h.frames(build, 2);
        t = find_thumb(h);
        CHECK(near_eq(t.y, start_y, 0.05f));
        std::fprintf(stderr, "  %s: ok\n", what);
    };

    {   // a child region with a hundred thousand rows
        harness h;
        const auto build = [&] {
            if (auto w = h.ui.window("l", {50, 50}, {400, 300}, plain_window)) {
                if (auto rows = h.ui.child("rows", {0.0f, 170.0f}, child_flags::frame)) {
                    list_clipper clip(h.ui, 100000, h.ui.font().line_height(0) + 8.0f);
                    while (clip.step()) { for (int i = clip.begin(); i < clip.end(); ++i) { (void)h.ui.selectable("row " + std::to_string(i), false); } }
                }
            }
        };
        drag_test(h, build, "child region, 100 000 rows", true);

        // a click on the track beside the thumb puts the thumb there (its middle under the pointer)
        const thumb_shape t = find_thumb(h);
        const f32 target = t.y + 90.0f;
        h.click({t.x, target}, build);
        const thumb_shape moved = find_thumb(h);
        CHECK(near_eq(moved.y + moved.h * 0.5f, target, 1.0f));

        // and a click near the bottom of the track goes there
        const f32 low = t.y + 140.0f;
        h.click({t.x, low}, build);
        h.frames(build, 2);
        const thumb_shape end = find_thumb(h);
        CHECK(near_eq(end.y + end.h * 0.5f, low, 1.0f) && end.y > moved.y + 20.0f);
    }
    {   // a window taller than its frame
        harness h;
        const auto build = [&] {
            if (auto w = h.ui.window("w", {50, 50}, {300, 260}, plain_window)) {
                for (int i = 0; i < 300; ++i) { (void)h.ui.custom_item("r" + std::to_string(i), {100.0f, 20.0f}); }
            }
        };
        drag_test(h, build, "window", false);
    }
    {   // a table with a scrolling body
        harness h;
        const auto build = [&] {
            if (auto w = h.ui.window("t", {50, 50}, {400, 320}, plain_window)) {
                if (h.ui.begin_table("tab", 2, table_default, 200.0f)) {
                    h.ui.table_setup_column("a");
                    h.ui.table_setup_column("b");
                    (void)h.ui.table_headers_row();
                    for (int i = 0; i < 400; ++i) {
                        if (h.ui.table_next_row()) {
                            h.ui.table_next_column(); h.ui.textf("row {}", i);
                            h.ui.table_next_column(); h.ui.textf("{}", i * 2);
                        }
                    }
                    h.ui.end_table();
                }
            }
        };
        drag_test(h, build, "table", false);
    }
}

} // namespace

void run_widgets_tests()
{
    test_images();
    test_nested_tables();
    test_texture_image();
    test_charts();
    test_plots();
    test_number_widgets();
    test_tabs();
    test_datetime_pickers();
    test_lists_and_tables();
    test_scrollbar_drag();
    test_item_list();
    test_ui_strings();
}
