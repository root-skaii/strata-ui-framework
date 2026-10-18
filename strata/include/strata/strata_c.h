/* C interface to strata: the context, the common widgets, the draw data and the software renderer.
 *
 * every function takes the context it works on; nothing here throws. bool results are 0 / 1. strings are utf-8,
 * nul-terminated. the draw data and font atlas pointers stay valid until the next strata_begin_frame.
 *
 *     strata_context* ui = strata_context_create(NULL);
 *     strata_input in = {0};
 *     in.display_w = 800; in.display_h = 600; in.delta_time = 1.0f / 60.0f;
 *     strata_begin_frame(ui, &in);
 *     if (strata_begin_window(ui, "hello", 20, 20, 300, 160, 0)) {
 *         strata_text(ui, "from c");
 *         if (strata_button(ui, "ok")) { ... }
 *     }
 *     strata_end_window(ui);
 *     strata_end_frame(ui);
 *     strata_draw_data dd;
 *     strata_get_draw_data(ui, &dd);   // hand to a renderer, or to strata_software_render()
 */
#ifndef STRATA_C_H
#define STRATA_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct strata_context strata_context;
typedef struct strata_software_renderer strata_software_renderer;

typedef struct strata_vec2 { float x, y; } strata_vec2;
typedef struct strata_rect { float min_x, min_y, max_x, max_y; } strata_rect;
/* straight-alpha 8-bit channels */
typedef struct strata_color { uint8_t r, g, b, a; } strata_color;

/* ---- setup ---------------------------------------------------------------------------------------------------- */

typedef struct strata_config {
    const char* font_face;   /* installed family; NULL: "Segoe UI" */
    float       font_size;   /* logical pixels; 0: 14 */
    const char* theme;       /* built-in theme name ("midnight", "light", "nord", ...); NULL: midnight */
} strata_config;

/* NULL config = all defaults. returns NULL if the font cannot be built or the theme name is unknown */
strata_context* strata_context_create(const strata_config* config);
void            strata_context_destroy(strata_context* ui);
int             strata_set_theme(strata_context* ui, const char* name);

/* ---- frame ---------------------------------------------------------------------------------------------------- */

#define STRATA_MAX_KEYS 32

typedef struct strata_input {
    strata_vec2 mouse_pos;
    uint8_t     mouse_down[3];   /* left, right, middle */
    float       wheel;           /* notches, positive = up */
    float       delta_time;      /* seconds since the previous frame */
    float       display_w, display_h; /* physical pixels */
    uint8_t     ctrl, shift, alt;
    const char* typed_utf8;      /* text typed since the last frame, or NULL */
    uint32_t    typed_len;       /* bytes */
    /* keys pressed since the last frame, in order; values are Win32 virtual-key codes (see strata::key) */
    uint32_t    keys[STRATA_MAX_KEYS];
    uint32_t    key_count;
} strata_input;

void strata_begin_frame(strata_context* ui, const strata_input* input);
void strata_end_frame(strata_context* ui);

int strata_want_capture_mouse(const strata_context* ui);
int strata_want_text_input(const strata_context* ui);
/* 0 arrow, 1 text, 2 hand, 3 not allowed, 4 resize ew, 5 resize ns, 6 resize nwse, 7 resize nesw */
int strata_cursor(const strata_context* ui);

/* ---- widgets -------------------------------------------------------------------------------------------------- */

/* bit values of strata::window_flags */
#define STRATA_WINDOW_RESIZABLE    1
#define STRATA_WINDOW_NO_TITLE_BAR 2
#define STRATA_WINDOW_NO_COLLAPSE  4
#define STRATA_WINDOW_NO_MOVE      8

/* false while collapsed. strata_end_window must follow every strata_begin_window, whatever it returned */
int  strata_begin_window(strata_context* ui, const char* title, float x, float y, float width, float height, uint32_t flags);
void strata_end_window(strata_context* ui);

void strata_text(strata_context* ui, const char* text);
void strata_text_wrapped(strata_context* ui, const char* text);
void strata_text_dim(strata_context* ui, const char* text);
void strata_text_colored(strata_context* ui, strata_color color, const char* text);

/* these return 1 when clicked / changed */
int strata_button(strata_context* ui, const char* label);
int strata_checkbox(strata_context* ui, const char* label, int* value);
int strata_slider_float(strata_context* ui, const char* label, float* value, float lo, float hi);
int strata_slider_int(strata_context* ui, const char* label, int* value, int lo, int hi);
int strata_drag_float(strata_context* ui, const char* label, float* value, float speed, float lo, float hi);
int strata_input_float(strata_context* ui, const char* label, float* value);
int strata_input_int(strata_context* ui, const char* label, int* value);
/* edits `buffer` in place (capacity includes the terminator) */
int strata_input_text(strata_context* ui, const char* label, char* buffer, size_t capacity);
int strata_combo(strata_context* ui, const char* label, int* current, const char* const* items, int count);
int strata_color_edit(strata_context* ui, const char* label, strata_color* color);
int strata_selectable(strata_context* ui, const char* label, int selected);
void strata_progress_bar(strata_context* ui, float fraction, const char* overlay);

void strata_separator(strata_context* ui);
void strata_spacing(strata_context* ui);
void strata_same_line(strata_context* ui);
void strata_tooltip(strata_context* ui, const char* text);
int  strata_item_hovered(const strata_context* ui);

/* when this returns 1, strata_tree_pop must follow */
int  strata_tree_node(strata_context* ui, const char* label);
void strata_tree_pop(strata_context* ui);
/* when this returns 1, strata_end_child must follow. size 0 = fill */
int  strata_begin_child(strata_context* ui, const char* id, float width, float height);
void strata_end_child(strata_context* ui);

/* tables: strata_end_table follows a 1 from strata_begin_table. flag bits: striped 1, borders 2, resizable 4, row_hover 8 */
int  strata_begin_table(strata_context* ui, const char* id, uint32_t columns, uint32_t flags, float height);
void strata_table_setup_column(strata_context* ui, const char* label, float fixed_width);
void strata_table_headers_row(strata_context* ui);
int  strata_table_next_row(strata_context* ui);
int  strata_table_next_column(strata_context* ui);
void strata_end_table(strata_context* ui);

/* a tab strip; returns 1 when `selected` changed. the caller draws the content of the selected tab */
int strata_tab_bar(strata_context* ui, const char* id, const char* const* labels, int count, int* selected);

/* menus: the end call follows a 1 from the matching begin */
int  strata_begin_main_menu_bar(strata_context* ui);
void strata_end_main_menu_bar(strata_context* ui);
int  strata_begin_menu(strata_context* ui, const char* label);
void strata_end_menu(strata_context* ui);
int  strata_menu_item(strata_context* ui, const char* label, const char* shortcut);

/* popups: open once (e.g. on a right-click), then call strata_begin_popup every frame */
void strata_open_popup(strata_context* ui, const char* id, strata_vec2 pos);
int  strata_begin_popup(strata_context* ui, const char* id);
void strata_end_popup(strata_context* ui);
void strata_close_popup(strata_context* ui);

void strata_plot_lines(strata_context* ui, const char* label, const float* values, size_t count, float width, float height);
void strata_plot_histogram(strata_context* ui, const char* label, const float* values, size_t count, float width, float height);

/* what the last widget did; button 0 left, 1 right, 2 middle */
int strata_item_clicked(const strata_context* ui, int button);
int strata_item_active(const strata_context* ui);
int strata_window_focused(const strata_context* ui);
strata_vec2 strata_mouse_pos(const strata_context* ui);
uint64_t strata_frame_index(const strata_context* ui);

typedef struct strata_frame_stats {
    uint32_t vertices, indices, draw_calls;
    double   ui_ms; /* begin_frame + end_frame cpu time of the last frame */
} strata_frame_stats;
void strata_get_frame_stats(const strata_context* ui, strata_frame_stats* out);

/* the debug windows; `open` is read and written (the windows have a close state of their own) */
void strata_debug_metrics_window(strata_context* ui, int* open);
void strata_debug_inspector_window(strata_context* ui, int* open);

/* ids: widgets with the same label need distinct scopes */
void strata_push_id_str(strata_context* ui, const char* id);
void strata_push_id_int(strata_context* ui, int id);
void strata_pop_id(strata_context* ui);

/* ---- node editor (see strata/node_editor.hpp) ------------------------------------------------------------------- */

typedef struct strata_node_editor strata_node_editor;

strata_node_editor* strata_node_editor_create(void);
void                strata_node_editor_destroy(strata_node_editor* ed);

/* when strata_node_editor_begin returns 1, submit nodes and links, then strata_node_editor_end */
int  strata_node_editor_begin(strata_node_editor* ed, strata_context* ui, const char* label, float width, float height);
void strata_node_editor_end(strata_node_editor* ed);
/* `pos` points at two floats (x, y in canvas coordinates) that dragging updates */
void strata_node_begin(strata_node_editor* ed, uint64_t id, const char* title, float* pos);
void strata_node_input_pin(strata_node_editor* ed, uint64_t pin_id, const char* label, uint32_t type);
void strata_node_output_pin(strata_node_editor* ed, uint64_t pin_id, const char* label, uint32_t type);
void strata_node_text(strata_node_editor* ed, const char* text);
void strata_node_end(strata_node_editor* ed);
void strata_node_link(strata_node_editor* ed, uint64_t output_pin_id, uint64_t input_pin_id);

/* results of the last end: 1 and the output / input pin ids when the user made or removed a link */
int strata_node_link_created(const strata_node_editor* ed, uint64_t* from, uint64_t* to);
int strata_node_link_deleted(const strata_node_editor* ed, uint64_t* from, uint64_t* to);
int strata_node_delete_requested(const strata_node_editor* ed);
size_t          strata_node_selected_count(const strata_node_editor* ed);
const uint64_t* strata_node_selected_ids(const strata_node_editor* ed);
void            strata_node_clear_selection(strata_node_editor* ed);

/* right-click: target 1 canvas, 2 node (`node`), 3 link (`from`, `to`) */
typedef struct strata_node_request {
    int         target;
    uint64_t    node, from, to;
    strata_vec2 screen, canvas;
} strata_node_request;
int strata_node_context_requested(const strata_node_editor* ed, strata_node_request* out);

void strata_node_editor_get_view(const strata_node_editor* ed, float* pan_x, float* pan_y, float* zoom);
void strata_node_editor_set_view(strata_node_editor* ed, float pan_x, float pan_y, float zoom);

/* ---- draw data ------------------------------------------------------------------------------------------------ */

/* same layouts as strata::vertex, draw_cmd and shape_record (checked at compile time in the library) */
typedef struct strata_vertex {
    strata_vec2  pos;      /* physical pixels */
    uint16_t     u, v;     /* atlas coordinates in 1/32767; bit 15 of v marks a shape quad (see strata/draw_list.hpp) */
    strata_color col;
} strata_vertex;

typedef struct strata_draw_cmd {
    strata_rect clip;      /* physical pixels */
    uint32_t    idx_offset, idx_count, vtx_offset;
    uint32_t    texture;   /* 0: font atlas */
    float       blur;      /* > 0: backdrop command */
} strata_draw_cmd;

typedef struct strata_shape {
    float    center[2], half_size[2], radius[4];
    uint32_t fill_top, fill_bottom, border, shadow; /* rgba8, r in the low byte */
    float    border_width, shadow_blur, shadow_offset[2], gradient_dir[2], gradient_kind, extra;
} strata_shape;

typedef struct strata_draw_data {
    const strata_vertex*   vertices;  size_t vertex_count;
    const uint16_t*        indices;   size_t index_count;   /* relative to the command's vtx_offset */
    const strata_draw_cmd* commands;  size_t command_count;
    const strata_shape*    shapes;    size_t shape_count;
    float                  display_w, display_h;
    float                  text_contrast;
} strata_draw_data;

void strata_get_draw_data(const strata_context* ui, strata_draw_data* out);

/* the font atlas: 8-bit coverage, row-major. for renderers other than the built-in ones */
void strata_get_font_atlas(const strata_context* ui, const uint8_t** pixels, uint32_t* width, uint32_t* height);

/* ---- software renderer: draws strata_draw_data on the cpu, no gpu or window needed -------------------------------- */

strata_software_renderer* strata_software_create(const strata_context* ui);
void strata_software_destroy(strata_software_renderer* r);
void strata_software_resize(strata_software_renderer* r, uint32_t width, uint32_t height);
void strata_software_clear(strata_software_renderer* r, strata_color color);
/* draws the context's last finished frame over the current contents (and resizes to its display size) */
void strata_software_render(strata_software_renderer* r, const strata_context* ui);
uint32_t strata_software_width(const strata_software_renderer* r);
uint32_t strata_software_height(const strata_software_renderer* r);
/* straight-alpha rgba8, top row first, width * height * 4 bytes. false if `capacity` is too small */
int strata_software_read_pixels(const strata_software_renderer* r, uint8_t* out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* STRATA_C_H */
