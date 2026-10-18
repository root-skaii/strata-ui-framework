/* compiled as C: checks that strata_c.h is valid C and that the C interface drives a whole frame */

#include <strata/strata_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static int clicks = 0;
static int flag = 1;
static float level = 0.25f;
static int count = 3;
static int choice = 1;
static char name[32] = "strata";
static int tab = 0;
static strata_node_editor* graph = NULL;
static float pos_a[2] = {20, 20};
static float pos_b[2] = {200, 30};

static void build(strata_context* ui)
{
    static const char* const items[] = {"one", "two", "three"};
    if (strata_begin_window(ui, "c api", 20, 20, 420, 640, 0)) {
        strata_text(ui, "drawn from c");
        strata_text_colored(ui, (strata_color){255, 180, 60, 255}, "colored");
        if (strata_button(ui, "press")) { ++clicks; }
        strata_checkbox(ui, "enabled", &flag);
        strata_slider_float(ui, "level", &level, 0.0f, 1.0f);
        strata_slider_int(ui, "count", &count, 0, 10);
        strata_input_text(ui, "name", name, sizeof name);
        strata_combo(ui, "choice", &choice, items, 3);
        strata_progress_bar(ui, level, NULL);
        if (strata_tree_node(ui, "details")) {
            strata_text_dim(ui, "inside the node");
            strata_tree_pop(ui);
        }
        if (strata_begin_table(ui, "table", 2, 3, 0)) {
            strata_table_setup_column(ui, "name", 100);
            strata_table_setup_column(ui, "value", 0);
            strata_table_headers_row(ui);
            strata_table_next_row(ui);
            strata_table_next_column(ui);
            strata_text(ui, "row");
            strata_table_next_column(ui);
            strata_text(ui, "42");
            strata_end_table(ui);
        }
        {
            static const char* const tab_labels[] = {"first", "second"};
            strata_tab_bar(ui, "tabs", tab_labels, 2, &tab);
            static const float samples[] = {1, 3, 2, 5, 4, 6};
            strata_plot_lines(ui, "samples", samples, 6, 0, 60);
        }
        if (strata_node_editor_begin(graph, ui, "graph", 0, 160)) {
            strata_node_begin(graph, 1, "A", pos_a);
            strata_node_output_pin(graph, 12, "out", 0);
            strata_node_end(graph);
            strata_node_begin(graph, 2, "B", pos_b);
            strata_node_input_pin(graph, 21, "in", 0);
            strata_node_text(graph, "body");
            strata_node_end(graph);
            strata_node_link(graph, 12, 21);
            strata_node_editor_end(graph);
        }
        strata_push_id_int(ui, 7);
        strata_separator(ui);
        strata_pop_id(ui);
    }
    strata_end_window(ui);
}

static void frame(strata_context* ui, strata_input* in)
{
    strata_begin_frame(ui, in);
    build(ui);
    strata_end_frame(ui);
}

int main(void)
{
    strata_config bad = {0};
    bad.theme = "no such theme";
    CHECK(strata_context_create(&bad) == NULL);

    graph = strata_node_editor_create();
    CHECK(graph != NULL);
    strata_context* ui = strata_context_create(NULL);
    CHECK(ui != NULL);
    if (ui == NULL) { return 1; }
    CHECK(strata_set_theme(ui, "nord"));
    CHECK(!strata_set_theme(ui, "no such theme"));

    strata_input in;
    memset(&in, 0, sizeof in);
    in.display_w = 800;
    in.display_h = 600;
    in.delta_time = 1.0f / 60.0f;
    in.mouse_pos.x = -100;
    in.mouse_pos.y = -100;
    frame(ui, &in);
    frame(ui, &in);

    strata_draw_data dd;
    strata_get_draw_data(ui, &dd);
    CHECK(dd.vertex_count > 0 && dd.index_count > 0 && dd.command_count > 0);
    CHECK(dd.display_w == 800 && dd.display_h == 600);

    const uint8_t* atlas = NULL;
    uint32_t aw = 0, ah = 0;
    strata_get_font_atlas(ui, &atlas, &aw, &ah);
    CHECK(atlas != NULL && aw > 0 && ah > 0);

    strata_software_renderer* sw = strata_software_create(ui);
    CHECK(sw != NULL);
    if (sw != NULL) {
        strata_software_resize(sw, 800, 600);
        strata_color bg = {10, 12, 16, 255};
        strata_software_clear(sw, bg);
        strata_software_render(sw, ui);
        CHECK(strata_software_width(sw) == 800 && strata_software_height(sw) == 600);

        size_t size = (size_t)800 * 600 * 4;
        uint8_t* px = (uint8_t*)malloc(size);
        CHECK(px != NULL && strata_software_read_pixels(sw, px, size));
        CHECK(!strata_software_read_pixels(sw, px, 16));
        size_t changed = 0;
        for (size_t i = 0; px != NULL && i < size; i += 4) {
            changed += (px[i] != 10 || px[i + 1] != 12 || px[i + 2] != 16) ? 1u : 0u;
        }
        CHECK(changed > 5000);
        CHECK(px != NULL && px[(size_t)(550 * 800 + 700) * 4 + 3] == 255); /* far from the window: the clear colour */
        free(px);
        strata_software_destroy(sw);
    }

    /* a click on the button: find it by trying the window's content area */
    in.mouse_pos.x = 60;
    for (int y = 70; y < 140 && clicks == 0; y += 4) {
        in.mouse_pos.y = (float)y;
        in.mouse_down[0] = 0;
        frame(ui, &in);
        frame(ui, &in);
        in.mouse_down[0] = 1;
        frame(ui, &in);
        in.mouse_down[0] = 0;
        frame(ui, &in);
    }
    CHECK(clicks > 0);

    {
        strata_frame_stats st;
        strata_get_frame_stats(ui, &st);
        CHECK(st.vertices > 0 && st.draw_calls > 0);
        float px, py, zoom;
        strata_node_editor_set_view(graph, 5, 6, 1.5f);
        strata_node_editor_get_view(graph, &px, &py, &zoom);
        CHECK(px == 5 && py == 6 && zoom == 1.5f);
    }
    strata_node_editor_destroy(graph);
    strata_context_destroy(ui);
    if (failures == 0) { fprintf(stderr, "c api: ok\n"); }
    return failures == 0 ? 0 : 1;
}
