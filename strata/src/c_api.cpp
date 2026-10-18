#include "strata/strata_c.h"

#include "strata/backend/software.hpp"
#include "strata/context.hpp"
#include "strata/node_editor.hpp"
#include "strata/themes.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

using namespace strata;

struct strata_context {
    context ui;
};

struct strata_software_renderer {
    software_renderer renderer;
};

static_assert(sizeof(strata_vertex) == sizeof(vertex) && offsetof(strata_vertex, u) == offsetof(vertex, u) &&
              offsetof(strata_vertex, col) == offsetof(vertex, col));
static_assert(sizeof(strata_draw_cmd) == sizeof(draw_cmd) && offsetof(strata_draw_cmd, idx_offset) == offsetof(draw_cmd, idx_offset) &&
              offsetof(strata_draw_cmd, texture) == offsetof(draw_cmd, texture) && offsetof(strata_draw_cmd, blur) == offsetof(draw_cmd, blur));
static_assert(sizeof(strata_shape) == sizeof(shape_record) && offsetof(strata_shape, fill_top) == offsetof(shape_record, fill_top) &&
              offsetof(strata_shape, border_width) == offsetof(shape_record, border_width) &&
              offsetof(strata_shape, extra) == offsetof(shape_record, extra));
static_assert(sizeof(strata_color) == sizeof(color));

namespace {

[[nodiscard]] std::string_view sv(const char* s) noexcept { return s != nullptr ? std::string_view{s} : std::string_view{}; }
[[nodiscard]] color to_color(strata_color c) noexcept { return {c.r, c.g, c.b, c.a}; }
[[nodiscard]] context& ctx(strata_context* c) noexcept { return c->ui; }

} // namespace

extern "C" {

strata_context* strata_context_create(const strata_config* config)
{
    context_config cfg;
    if (config != nullptr) {
        if (config->font_face != nullptr) { cfg.font.face = config->font_face; }
        if (config->font_size > 0.0f) { cfg.font.pixel_height = config->font_size; }
        if (config->theme != nullptr && !themes::by_name(config->theme, cfg.theme)) { return nullptr; }
    }
    auto made = context::create(cfg);
    if (!made) { return nullptr; }
    return new (std::nothrow) strata_context{std::move(*made)};
}

void strata_context_destroy(strata_context* ui) { delete ui; }

int strata_set_theme(strata_context* ui, const char* name)
{
    style s;
    if (ui == nullptr || !themes::by_name(sv(name), s)) { return 0; }
    ui->ui.theme() = s;
    return 1;
}

void strata_begin_frame(strata_context* ui, const strata_input* input)
{
    input_state in;
    in.mouse_pos     = {input->mouse_pos.x, input->mouse_pos.y};
    in.mouse_down    = {input->mouse_down[0] != 0, input->mouse_down[1] != 0, input->mouse_down[2] != 0};
    in.wheel         = input->wheel;
    in.delta_time    = input->delta_time > 0.0f ? input->delta_time : in.delta_time;
    in.display_size  = {input->display_w, input->display_h};
    in.ctrl          = input->ctrl != 0;
    in.shift         = input->shift != 0;
    in.alt           = input->alt != 0;
    if (input->typed_utf8 != nullptr) {
        in.typed_len = static_cast<u32>(std::min<std::size_t>(input->typed_len, in.typed.size()));
        std::memcpy(in.typed.data(), input->typed_utf8, in.typed_len);
    }
    for (u32 i = 0; i < std::min<u32>(input->key_count, STRATA_MAX_KEYS); ++i) {
        in.press(static_cast<key>(input->keys[i]), in.ctrl, in.shift, in.alt);
    }
    ui->ui.begin_frame(std::move(in));
}

void strata_end_frame(strata_context* ui) { ui->ui.end_frame(); }

int strata_want_capture_mouse(const strata_context* ui) { return ui->ui.want_capture_mouse(); }
int strata_want_text_input(const strata_context* ui) { return ui->ui.want_text_input(); }
int strata_cursor(const strata_context* ui) { return static_cast<int>(ui->ui.cursor()); }

int strata_begin_window(strata_context* ui, const char* title, float x, float y, float width, float height, uint32_t flags)
{
    return ctx(ui).begin_window(sv(title), {x, y}, {width, height}, static_cast<window_flags>(flags));
}
void strata_end_window(strata_context* ui) { ctx(ui).end_window(); }

void strata_text(strata_context* ui, const char* text) { ctx(ui).text(sv(text)); }
void strata_text_wrapped(strata_context* ui, const char* text) { ctx(ui).text_wrapped(sv(text)); }
void strata_text_dim(strata_context* ui, const char* text) { ctx(ui).text_dim(sv(text)); }
void strata_text_colored(strata_context* ui, strata_color c, const char* text) { ctx(ui).text_colored(to_color(c), sv(text)); }

int strata_button(strata_context* ui, const char* label) { return ctx(ui).button(sv(label)); }

int strata_checkbox(strata_context* ui, const char* label, int* value)
{
    bool b = *value != 0;
    const bool changed = ctx(ui).checkbox(sv(label), b);
    *value = b ? 1 : 0;
    return changed;
}

int strata_slider_float(strata_context* ui, const char* label, float* value, float lo, float hi)
{
    return ctx(ui).slider<float>(sv(label), *value, lo, hi);
}
int strata_slider_int(strata_context* ui, const char* label, int* value, int lo, int hi)
{
    return ctx(ui).slider<int>(sv(label), *value, lo, hi);
}
int strata_drag_float(strata_context* ui, const char* label, float* value, float speed, float lo, float hi)
{
    return ctx(ui).drag_float(sv(label), *value, speed, lo, hi);
}
int strata_input_float(strata_context* ui, const char* label, float* value) { return ctx(ui).input_float(sv(label), *value); }
int strata_input_int(strata_context* ui, const char* label, int* value) { return ctx(ui).input_int(sv(label), *value); }
int strata_input_text(strata_context* ui, const char* label, char* buffer, size_t capacity)
{
    return ctx(ui).input_text(sv(label), buffer, capacity);
}

int strata_combo(strata_context* ui, const char* label, int* current, const char* const* items, int count)
{
    std::vector<std::string_view> list;
    list.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) { list.push_back(sv(items[i])); }
    return ctx(ui).combo(sv(label), *current, item_list{list.data(), list.size()});
}

int strata_color_edit(strata_context* ui, const char* label, strata_color* c)
{
    color value = to_color(*c);
    const bool changed = ctx(ui).color_edit(sv(label), value);
    *c = {value.r, value.g, value.b, value.a};
    return changed;
}

int strata_selectable(strata_context* ui, const char* label, int selected) { return ctx(ui).selectable(sv(label), selected != 0); }
void strata_progress_bar(strata_context* ui, float fraction, const char* overlay) { ctx(ui).progress_bar(fraction, sv(overlay)); }

void strata_separator(strata_context* ui) { ctx(ui).separator(); }
void strata_spacing(strata_context* ui) { ctx(ui).spacing(); }
void strata_same_line(strata_context* ui) { ctx(ui).same_line(); }
void strata_tooltip(strata_context* ui, const char* text) { ctx(ui).tooltip(sv(text)); }
int  strata_item_hovered(const strata_context* ui) { return ui->ui.item_hovered(); }

int  strata_tree_node(strata_context* ui, const char* label) { return ctx(ui).tree_node(sv(label)); }
void strata_tree_pop(strata_context* ui) { ctx(ui).tree_pop(); }
int  strata_begin_child(strata_context* ui, const char* id, float width, float height)
{
    return ctx(ui).begin_child(sv(id), {width, height});
}
void strata_end_child(strata_context* ui) { ctx(ui).end_child(); }

void strata_push_id_str(strata_context* ui, const char* id) { ctx(ui).push_id(sv(id)); }
void strata_push_id_int(strata_context* ui, int id) { ctx(ui).push_id(id); }
void strata_pop_id(strata_context* ui) { ctx(ui).pop_id(); }

int strata_begin_table(strata_context* ui, const char* id, uint32_t columns, uint32_t flags, float height)
{
    return ctx(ui).begin_table(sv(id), columns, static_cast<table_flags>(flags), height);
}
void strata_table_setup_column(strata_context* ui, const char* label, float fixed_width) { ctx(ui).table_setup_column(sv(label), fixed_width); }
void strata_table_headers_row(strata_context* ui) { (void)ctx(ui).table_headers_row(); }
int  strata_table_next_row(strata_context* ui) { return ctx(ui).table_next_row(); }
int  strata_table_next_column(strata_context* ui) { return ctx(ui).table_next_column(); }
void strata_end_table(strata_context* ui) { ctx(ui).end_table(); }

int strata_tab_bar(strata_context* ui, const char* id, const char* const* labels, int count, int* selected)
{
    std::vector<tab_desc> tabs;
    tabs.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) { tabs.emplace_back(sv(labels[i])); }
    return ctx(ui).tab_bar(sv(id), tabs.data(), tabs.size(), *selected);
}

int  strata_begin_main_menu_bar(strata_context* ui) { return ctx(ui).begin_main_menu_bar(); }
void strata_end_main_menu_bar(strata_context* ui) { ctx(ui).end_main_menu_bar(); }
int  strata_begin_menu(strata_context* ui, const char* label) { return ctx(ui).begin_menu(sv(label)); }
void strata_end_menu(strata_context* ui) { ctx(ui).end_menu(); }
int  strata_menu_item(strata_context* ui, const char* label, const char* shortcut) { return ctx(ui).menu_item(sv(label), sv(shortcut)); }

void strata_open_popup(strata_context* ui, const char* id, strata_vec2 pos) { ctx(ui).open_popup(sv(id), {pos.x, pos.y}); }
int  strata_begin_popup(strata_context* ui, const char* id) { return ctx(ui).begin_popup(sv(id)); }
void strata_end_popup(strata_context* ui) { ctx(ui).end_popup(); }
void strata_close_popup(strata_context* ui) { ctx(ui).close_popup(); }

void strata_plot_lines(strata_context* ui, const char* label, const float* values, size_t count, float width, float height)
{
    ctx(ui).plot_lines(sv(label), {values, count}, {width, height});
}
void strata_plot_histogram(strata_context* ui, const char* label, const float* values, size_t count, float width, float height)
{
    ctx(ui).plot_histogram(sv(label), {values, count}, {width, height});
}

int strata_item_clicked(const strata_context* ui, int button)
{
    return ui->ui.item_clicked(button == 1 ? mouse_button::right : (button == 2 ? mouse_button::middle : mouse_button::left));
}
int strata_item_active(const strata_context* ui) { return ui->ui.item_active(); }
int strata_window_focused(const strata_context* ui) { return ui->ui.window_focused(); }
strata_vec2 strata_mouse_pos(const strata_context* ui)
{
    const vec2 p = ui->ui.mouse_pos();
    return {p.x, p.y};
}
uint64_t strata_frame_index(const strata_context* ui) { return ui->ui.frame_index(); }

void strata_get_frame_stats(const strata_context* ui, strata_frame_stats* out)
{
    const frame_stats& st = ui->ui.stats();
    out->vertices   = st.vertices;
    out->indices    = st.indices;
    out->draw_calls = st.draw_calls;
    out->ui_ms      = st.begin_frame_ms + st.end_frame_ms;
}

void strata_debug_metrics_window(strata_context* ui, int* open)
{
    bool b = *open != 0;
    ctx(ui).debug_metrics_window(b);
    *open = b ? 1 : 0;
}
void strata_debug_inspector_window(strata_context* ui, int* open)
{
    bool b = *open != 0;
    ctx(ui).debug_inspector_window(b);
    *open = b ? 1 : 0;
}

struct strata_node_editor {
    node_editor editor;
};

static_assert(sizeof(vec2) == 2 * sizeof(float));

strata_node_editor* strata_node_editor_create(void) { return new (std::nothrow) strata_node_editor; }
void strata_node_editor_destroy(strata_node_editor* ed) { delete ed; }

int strata_node_editor_begin(strata_node_editor* ed, strata_context* ui, const char* label, float width, float height)
{
    return ed->editor.begin(ctx(ui), sv(label), {width, height});
}
void strata_node_editor_end(strata_node_editor* ed) { ed->editor.end(); }
void strata_node_begin(strata_node_editor* ed, uint64_t id, const char* title, float* pos)
{
    ed->editor.begin_node(id, sv(title), *reinterpret_cast<vec2*>(pos));
}
void strata_node_input_pin(strata_node_editor* ed, uint64_t pin_id, const char* label, uint32_t type)
{
    ed->editor.input_pin(pin_id, sv(label), type);
}
void strata_node_output_pin(strata_node_editor* ed, uint64_t pin_id, const char* label, uint32_t type)
{
    ed->editor.output_pin(pin_id, sv(label), type);
}
void strata_node_text(strata_node_editor* ed, const char* text) { ed->editor.node_text(sv(text)); }
void strata_node_end(strata_node_editor* ed) { ed->editor.end_node(); }
void strata_node_link(strata_node_editor* ed, uint64_t output_pin_id, uint64_t input_pin_id) { ed->editor.link(output_pin_id, input_pin_id); }

int strata_node_link_created(const strata_node_editor* ed, uint64_t* from, uint64_t* to)
{
    node_link l;
    const bool ok = ed->editor.link_created(l);
    *from = l.from;
    *to   = l.to;
    return ok;
}
int strata_node_link_deleted(const strata_node_editor* ed, uint64_t* from, uint64_t* to)
{
    node_link l;
    const bool ok = ed->editor.link_deleted(l);
    *from = l.from;
    *to   = l.to;
    return ok;
}
int strata_node_delete_requested(const strata_node_editor* ed) { return ed->editor.delete_requested(); }
size_t strata_node_selected_count(const strata_node_editor* ed) { return ed->editor.selected_nodes().size(); }
const uint64_t* strata_node_selected_ids(const strata_node_editor* ed) { return ed->editor.selected_nodes().data(); }
void strata_node_clear_selection(strata_node_editor* ed) { ed->editor.clear_selection(); }

int strata_node_context_requested(const strata_node_editor* ed, strata_node_request* out)
{
    node_context_request r;
    const bool ok = ed->editor.context_requested(r);
    out->target = static_cast<int>(r.target);
    out->node   = r.node;
    out->from   = r.link.from;
    out->to     = r.link.to;
    out->screen = {r.screen.x, r.screen.y};
    out->canvas = {r.canvas.x, r.canvas.y};
    return ok;
}

void strata_node_editor_get_view(const strata_node_editor* ed, float* pan_x, float* pan_y, float* zoom)
{
    *pan_x = ed->editor.pan.x;
    *pan_y = ed->editor.pan.y;
    *zoom  = ed->editor.zoom;
}
void strata_node_editor_set_view(strata_node_editor* ed, float pan_x, float pan_y, float zoom)
{
    ed->editor.pan  = {pan_x, pan_y};
    ed->editor.zoom = zoom;
}

void strata_get_draw_data(const strata_context* ui, strata_draw_data* out)
{
    const draw_data d = ui->ui.render_data();
    out->vertices      = reinterpret_cast<const strata_vertex*>(d.vertices.data());
    out->vertex_count  = d.vertices.size();
    out->indices       = d.indices.data();
    out->index_count   = d.indices.size();
    out->commands      = reinterpret_cast<const strata_draw_cmd*>(d.commands.data());
    out->command_count = d.commands.size();
    out->shapes        = reinterpret_cast<const strata_shape*>(d.shapes.data());
    out->shape_count   = d.shapes.size();
    out->display_w     = d.display_size.x;
    out->display_h     = d.display_size.y;
    out->text_contrast = d.text_contrast;
}

void strata_get_font_atlas(const strata_context* ui, const uint8_t** pixels, uint32_t* width, uint32_t* height)
{
    const font_atlas& atlas = ui->ui.font();
    *pixels = atlas.pixels().data();
    *width  = atlas.width();
    *height = atlas.height();
}

strata_software_renderer* strata_software_create(const strata_context* ui)
{
    auto* r = new (std::nothrow) strata_software_renderer;
    if (r != nullptr && !r->renderer.create(ui->ui.font())) {
        delete r;
        return nullptr;
    }
    return r;
}

void strata_software_destroy(strata_software_renderer* r) { delete r; }
void strata_software_resize(strata_software_renderer* r, uint32_t width, uint32_t height) { r->renderer.resize(width, height); }
void strata_software_clear(strata_software_renderer* r, strata_color c) { r->renderer.clear(to_color(c)); }
void strata_software_render(strata_software_renderer* r, const strata_context* ui) { r->renderer.render(ui->ui.render_data()); }
uint32_t strata_software_width(const strata_software_renderer* r) { return r->renderer.width(); }
uint32_t strata_software_height(const strata_software_renderer* r) { return r->renderer.height(); }

int strata_software_read_pixels(const strata_software_renderer* r, uint8_t* out, size_t capacity)
{
    const std::size_t need = static_cast<std::size_t>(r->renderer.width()) * r->renderer.height() * 4;
    return capacity >= need && r->renderer.copy_straight({out, need});
}

} // extern "C"
