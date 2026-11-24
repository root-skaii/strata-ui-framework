//! Raw C FFI bindings matching `strata/include/strata/strata_c.h`.
#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_int, c_void};

#[repr(C)]
pub struct strata_context {
    _private: [u8; 0],
}

#[repr(C)]
pub struct strata_software_renderer {
    _private: [u8; 0],
}

#[repr(C)]
pub struct strata_gl_renderer {
    _private: [u8; 0],
}

#[repr(C)]
pub struct strata_node_editor {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct strata_vec2 {
    pub x: f32,
    pub y: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct strata_rect {
    pub min_x: f32,
    pub min_y: f32,
    pub max_x: f32,
    pub max_y: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct strata_color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct strata_config {
    pub font_face: *const c_char,
    pub font_size: f32,
    pub theme: *const c_char,
}

impl Default for strata_config {
    fn default() -> Self {
        Self {
            font_face: std::ptr::null(),
            font_size: 0.0,
            theme: std::ptr::null(),
        }
    }
}

pub const STRATA_MAX_KEYS: usize = 32;

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct strata_input {
    pub mouse_pos: strata_vec2,
    pub mouse_down: [u8; 3],
    pub wheel: f32,
    pub delta_time: f32,
    pub display_w: f32,
    pub display_h: f32,
    pub ctrl: u8,
    pub shift: u8,
    pub alt: u8,
    pub typed_utf8: *const c_char,
    pub typed_len: u32,
    pub keys: [u32; STRATA_MAX_KEYS],
    pub key_count: u32,
}

impl Default for strata_input {
    fn default() -> Self {
        Self {
            mouse_pos: strata_vec2::default(),
            mouse_down: [0; 3],
            wheel: 0.0,
            delta_time: 1.0 / 60.0,
            display_w: 1280.0,
            display_h: 720.0,
            ctrl: 0,
            shift: 0,
            alt: 0,
            typed_utf8: std::ptr::null(),
            typed_len: 0,
            keys: [0; STRATA_MAX_KEYS],
            key_count: 0,
        }
    }
}

pub const STRATA_WINDOW_RESIZABLE: u32 = 1;
pub const STRATA_WINDOW_NO_TITLE_BAR: u32 = 2;
pub const STRATA_WINDOW_NO_COLLAPSE: u32 = 4;
pub const STRATA_WINDOW_NO_MOVE: u32 = 8;
pub const STRATA_WINDOW_NO_BACKGROUND: u32 = 16;
pub const STRATA_WINDOW_DOCKABLE: u32 = 64;
pub const STRATA_WINDOW_NO_INPUTS: u32 = 256;
pub const STRATA_WINDOW_CLICK_THROUGH: u32 = 2048;

pub const STRATA_DOCK_CENTER: c_int = 0;
pub const STRATA_DOCK_LEFT: c_int = 1;
pub const STRATA_DOCK_RIGHT: c_int = 2;
pub const STRATA_DOCK_TOP: c_int = 3;
pub const STRATA_DOCK_BOTTOM: c_int = 4;

pub const STRATA_DOCK_SIDE_LEFT: c_int = 0;
pub const STRATA_DOCK_SIDE_RIGHT: c_int = 1;
pub const STRATA_DOCK_SIDE_TOP: c_int = 2;
pub const STRATA_DOCK_SIDE_BOTTOM: c_int = 3;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct strata_frame_stats {
    pub vertices: u32,
    pub indices: u32,
    pub draw_calls: u32,
    pub ui_ms: f64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct strata_node_request {
    pub target: c_int,
    pub node: u64,
    pub from: u64,
    pub to: u64,
    pub screen: strata_vec2,
    pub canvas: strata_vec2,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct strata_vertex {
    pub pos: strata_vec2,
    pub u: u16,
    pub v: u16,
    pub col: strata_color,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct strata_draw_cmd {
    pub clip: strata_rect,
    pub idx_offset: u32,
    pub idx_count: u32,
    pub vtx_offset: u32,
    pub texture: u32,
    pub blur: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct strata_shape {
    pub center: [f32; 2],
    pub half_size: [f32; 2],
    pub radius: [f32; 4],
    pub fill_top: u32,
    pub fill_bottom: u32,
    pub border: u32,
    pub shadow: u32,
    pub border_width: f32,
    pub shadow_blur: f32,
    pub shadow_offset: [f32; 2],
    pub gradient_dir: [f32; 2],
    pub gradient_kind: f32,
    pub extra: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct strata_draw_data {
    pub vertices: *const strata_vertex,
    pub vertex_count: usize,
    pub indices: *const u16,
    pub index_count: usize,
    pub commands: *const strata_draw_cmd,
    pub command_count: usize,
    pub shapes: *const strata_shape,
    pub shape_count: usize,
    pub display_w: f32,
    pub display_h: f32,
    pub text_contrast: f32,
}

impl Default for strata_draw_data {
    fn default() -> Self {
        Self {
            vertices: std::ptr::null(),
            vertex_count: 0,
            indices: std::ptr::null(),
            index_count: 0,
            commands: std::ptr::null(),
            command_count: 0,
            shapes: std::ptr::null(),
            shape_count: 0,
            display_w: 0.0,
            display_h: 0.0,
            text_contrast: 1.0,
        }
    }
}

pub type strata_gl_proc_loader = Option<unsafe extern "C" fn(*const c_char) -> *mut c_void>;

extern "C" {
    pub fn strata_context_create(config: *const strata_config) -> *mut strata_context;
    pub fn strata_context_destroy(ui: *mut strata_context);
    pub fn strata_set_theme(ui: *mut strata_context, name: *const c_char) -> c_int;

    pub fn strata_begin_frame(ui: *mut strata_context, input: *const strata_input);
    pub fn strata_end_frame(ui: *mut strata_context);

    pub fn strata_want_capture_mouse(ui: *const strata_context) -> c_int;
    pub fn strata_want_text_input(ui: *const strata_context) -> c_int;
    pub fn strata_cursor(ui: *const strata_context) -> c_int;

    pub fn strata_begin_window(
        ui: *mut strata_context,
        title: *const c_char,
        x: f32,
        y: f32,
        width: f32,
        height: f32,
        flags: u32,
    ) -> c_int;
    pub fn strata_end_window(ui: *mut strata_context);
    pub fn strata_set_window_pos(ui: *mut strata_context, title: *const c_char, x: f32, y: f32);
    pub fn strata_set_window_size(ui: *mut strata_context, title: *const c_char, width: f32, height: f32);
    pub fn strata_set_window_collapsed(ui: *mut strata_context, title: *const c_char, collapsed: c_int);
    pub fn strata_focus_window(ui: *mut strata_context, title: *const c_char);

    pub fn strata_set_scale(ui: *mut strata_context, scale: f32) -> c_int;
    pub fn strata_scale(ui: *const strata_context) -> f32;

    pub fn strata_text(ui: *mut strata_context, text: *const c_char);
    pub fn strata_text_wrapped(ui: *mut strata_context, text: *const c_char);
    pub fn strata_text_dim(ui: *mut strata_context, text: *const c_char);
    pub fn strata_text_colored(ui: *mut strata_context, color: strata_color, text: *const c_char);

    pub fn strata_button(ui: *mut strata_context, label: *const c_char) -> c_int;
    pub fn strata_checkbox(ui: *mut strata_context, label: *const c_char, value: *mut c_int) -> c_int;
    pub fn strata_toggle(ui: *mut strata_context, label: *const c_char, value: *mut c_int) -> c_int;
    pub fn strata_radio_button(ui: *mut strata_context, label: *const c_char, active: c_int) -> c_int;
    pub fn strata_segmented(
        ui: *mut strata_context,
        id: *const c_char,
        items: *const *const c_char,
        count: c_int,
        current: *mut c_int,
    ) -> c_int;
    pub fn strata_splitter(
        ui: *mut strata_context,
        id: *const c_char,
        size: *mut f32,
        min_size: f32,
        max_size: f32,
        vertical: c_int,
        length: f32,
    ) -> c_int;
    pub fn strata_collapsing_header(ui: *mut strata_context, label: *const c_char, default_open: c_int) -> c_int;
    pub fn strata_slider_float(
        ui: *mut strata_context,
        label: *const c_char,
        value: *mut f32,
        lo: f32,
        hi: f32,
    ) -> c_int;
    pub fn strata_slider_int(
        ui: *mut strata_context,
        label: *const c_char,
        value: *mut c_int,
        lo: c_int,
        hi: c_int,
    ) -> c_int;
    pub fn strata_range_slider(
        ui: *mut strata_context,
        label: *const c_char,
        lo_val: *mut f32,
        hi_val: *mut f32,
        lo: f32,
        hi: f32,
    ) -> c_int;
    pub fn strata_knob(
        ui: *mut strata_context,
        label: *const c_char,
        value: *mut f32,
        lo: f32,
        hi: f32,
        default_val: f32,
    ) -> c_int;
    pub fn strata_drag_float(
        ui: *mut strata_context,
        label: *const c_char,
        value: *mut f32,
        speed: f32,
        lo: f32,
        hi: f32,
    ) -> c_int;
    pub fn strata_drag_int(
        ui: *mut strata_context,
        label: *const c_char,
        value: *mut c_int,
        speed: f32,
        lo: c_int,
        hi: c_int,
    ) -> c_int;
    pub fn strata_input_float(ui: *mut strata_context, label: *const c_char, value: *mut f32) -> c_int;
    pub fn strata_input_int(ui: *mut strata_context, label: *const c_char, value: *mut c_int) -> c_int;
    pub fn strata_input_text(
        ui: *mut strata_context,
        label: *const c_char,
        buffer: *mut c_char,
        capacity: usize,
    ) -> c_int;
    pub fn strata_combo(
        ui: *mut strata_context,
        label: *const c_char,
        current: *mut c_int,
        items: *const *const c_char,
        count: c_int,
    ) -> c_int;
    pub fn strata_color_edit(ui: *mut strata_context, label: *const c_char, color: *mut strata_color) -> c_int;
    pub fn strata_selectable(ui: *mut strata_context, label: *const c_char, selected: c_int) -> c_int;
    pub fn strata_progress_bar(ui: *mut strata_context, fraction: f32, overlay: *const c_char);

    pub fn strata_separator(ui: *mut strata_context);
    pub fn strata_separator_text(ui: *mut strata_context, label: *const c_char);
    pub fn strata_spacing(ui: *mut strata_context);
    pub fn strata_same_line(ui: *mut strata_context);
    pub fn strata_same_line_right(ui: *mut strata_context, width: f32);
    pub fn strata_set_next_item_width(ui: *mut strata_context, width: f32);
    pub fn strata_content_width(ui: *const strata_context) -> f32;
    pub fn strata_display_size(ui: *const strata_context) -> strata_vec2;
    pub fn strata_push_indent(ui: *mut strata_context, px: f32);
    pub fn strata_pop_indent(ui: *mut strata_context);
    pub fn strata_begin_row(ui: *mut strata_context, columns: u32, gap: f32) -> c_int;
    pub fn strata_begin_row_cols(ui: *mut strata_context, specs: *const f32, count: u32, gap: f32) -> c_int;
    pub fn strata_row_next(ui: *mut strata_context) -> c_int;
    pub fn strata_end_row(ui: *mut strata_context);
    pub fn strata_begin_disabled(ui: *mut strata_context, disabled: c_int);
    pub fn strata_end_disabled(ui: *mut strata_context);
    pub fn strata_tooltip(ui: *mut strata_context, text: *const c_char);
    pub fn strata_begin_tooltip(ui: *mut strata_context, width: f32) -> c_int;
    pub fn strata_end_tooltip(ui: *mut strata_context);
    pub fn strata_item_hovered(ui: *const strata_context) -> c_int;

    pub fn strata_tree_node(ui: *mut strata_context, label: *const c_char) -> c_int;
    pub fn strata_tree_pop(ui: *mut strata_context);
    pub fn strata_begin_child(ui: *mut strata_context, id: *const c_char, width: f32, height: f32) -> c_int;
    pub fn strata_end_child(ui: *mut strata_context);

    pub fn strata_begin_table(
        ui: *mut strata_context,
        id: *const c_char,
        columns: u32,
        flags: u32,
        height: f32,
    ) -> c_int;
    pub fn strata_table_setup_column(ui: *mut strata_context, label: *const c_char, fixed_width: f32);
    pub fn strata_table_headers_row(ui: *mut strata_context);
    pub fn strata_table_next_row(ui: *mut strata_context) -> c_int;
    pub fn strata_table_next_column(ui: *mut strata_context) -> c_int;
    pub fn strata_end_table(ui: *mut strata_context);

    pub fn strata_tab_bar(
        ui: *mut strata_context,
        id: *const c_char,
        labels: *const *const c_char,
        count: c_int,
        selected: *mut c_int,
    ) -> c_int;

    pub fn strata_begin_main_menu_bar(ui: *mut strata_context) -> c_int;
    pub fn strata_end_main_menu_bar(ui: *mut strata_context);
    pub fn strata_begin_menu(ui: *mut strata_context, label: *const c_char) -> c_int;
    pub fn strata_end_menu(ui: *mut strata_context);
    pub fn strata_menu_item(ui: *mut strata_context, label: *const c_char, shortcut: *const c_char) -> c_int;

    pub fn strata_open_popup(ui: *mut strata_context, id: *const c_char, pos: strata_vec2);
    pub fn strata_begin_popup(ui: *mut strata_context, id: *const c_char) -> c_int;
    pub fn strata_end_popup(ui: *mut strata_context);
    pub fn strata_close_popup(ui: *mut strata_context);

    pub fn strata_plot_lines(
        ui: *mut strata_context,
        label: *const c_char,
        values: *const f32,
        count: usize,
        width: f32,
        height: f32,
    );
    pub fn strata_plot_histogram(
        ui: *mut strata_context,
        label: *const c_char,
        values: *const f32,
        count: usize,
        width: f32,
        height: f32,
    );

    pub fn strata_item_clicked(ui: *const strata_context, button: c_int) -> c_int;
    pub fn strata_item_active(ui: *const strata_context) -> c_int;
    pub fn strata_window_focused(ui: *const strata_context) -> c_int;
    pub fn strata_mouse_pos(ui: *const strata_context) -> strata_vec2;
    pub fn strata_frame_index(ui: *const strata_context) -> u64;
    pub fn strata_get_frame_stats(ui: *const strata_context, out: *mut strata_frame_stats);

    pub fn strata_debug_metrics_window(ui: *mut strata_context, open: *mut c_int);
    pub fn strata_debug_inspector_window(ui: *mut strata_context, open: *mut c_int);

    pub fn strata_push_id_str(ui: *mut strata_context, id: *const c_char);
    pub fn strata_push_id_int(ui: *mut strata_context, id: c_int);
    pub fn strata_pop_id(ui: *mut strata_context);

    pub fn strata_set_keyboard_nav(ui: *mut strata_context, enabled: c_int);
    pub fn strata_keyboard_nav(ui: *const strata_context) -> c_int;
    pub fn strata_keyboard_focus_id(ui: *const strata_context) -> u64;

    pub fn strata_dock_area(ui: *mut strata_context, space: *const c_char, area: strata_rect);
    pub fn strata_dock_edge(
        ui: *mut strata_context,
        space: *const c_char,
        side: c_int,
        size: f32,
        region: strata_rect,
    ) -> strata_rect;
    pub fn strata_floating_dock(
        ui: *mut strata_context,
        title: *const c_char,
        x: f32,
        y: f32,
        width: f32,
        height: f32,
        flags: u32,
    ) -> c_int;
    pub fn strata_dock_window(
        ui: *mut strata_context,
        title: *const c_char,
        zone: c_int,
        target: *const c_char,
        size: f32,
        space: *const c_char,
    ) -> c_int;
    pub fn strata_undock_window(ui: *mut strata_context, title: *const c_char);
    pub fn strata_window_docked(ui: *const strata_context, title: *const c_char) -> c_int;
    pub fn strata_dock_save_layout(ui: *const strata_context, out: *mut c_char, capacity: usize) -> usize;
    pub fn strata_dock_load_layout(ui: *mut strata_context, text: *const c_char) -> c_int;

    pub fn strata_node_editor_create() -> *mut strata_node_editor;
    pub fn strata_node_editor_destroy(ed: *mut strata_node_editor);
    pub fn strata_node_editor_begin(
        ed: *mut strata_node_editor,
        ui: *mut strata_context,
        label: *const c_char,
        width: f32,
        height: f32,
    ) -> c_int;
    pub fn strata_node_editor_end(ed: *mut strata_node_editor);
    pub fn strata_node_begin(ed: *mut strata_node_editor, id: u64, title: *const c_char, pos: *mut f32);
    pub fn strata_node_input_pin(ed: *mut strata_node_editor, pin_id: u64, label: *const c_char, ty: u32);
    pub fn strata_node_output_pin(ed: *mut strata_node_editor, pin_id: u64, label: *const c_char, ty: u32);
    pub fn strata_node_text(ed: *mut strata_node_editor, text: *const c_char);
    pub fn strata_node_end(ed: *mut strata_node_editor);
    pub fn strata_node_link(ed: *mut strata_node_editor, output_pin_id: u64, input_pin_id: u64);
    pub fn strata_node_link_created(ed: *const strata_node_editor, from: *mut u64, to: *mut u64) -> c_int;
    pub fn strata_node_link_deleted(ed: *const strata_node_editor, from: *mut u64, to: *mut u64) -> c_int;
    pub fn strata_node_delete_requested(ed: *const strata_node_editor) -> c_int;
    pub fn strata_node_selected_count(ed: *const strata_node_editor) -> usize;
    pub fn strata_node_selected_ids(ed: *const strata_node_editor) -> *const u64;
    pub fn strata_node_clear_selection(ed: *mut strata_node_editor);
    pub fn strata_node_context_requested(ed: *const strata_node_editor, out: *mut strata_node_request) -> c_int;
    pub fn strata_node_editor_get_view(
        ed: *const strata_node_editor,
        pan_x: *mut f32,
        pan_y: *mut f32,
        zoom: *mut f32,
    );
    pub fn strata_node_editor_set_view(ed: *mut strata_node_editor, pan_x: f32, pan_y: f32, zoom: f32);

    pub fn strata_get_draw_data(ui: *const strata_context, out: *mut strata_draw_data);
    pub fn strata_get_font_atlas(
        ui: *const strata_context,
        pixels: *mut *const u8,
        width: *mut u32,
        height: *mut u32,
    );

    pub fn strata_software_create(ui: *const strata_context) -> *mut strata_software_renderer;
    pub fn strata_software_destroy(r: *mut strata_software_renderer);
    pub fn strata_software_resize(r: *mut strata_software_renderer, width: u32, height: u32);
    pub fn strata_software_clear(r: *mut strata_software_renderer, color: strata_color);
    pub fn strata_software_render(r: *mut strata_software_renderer, ui: *const strata_context);
    pub fn strata_software_width(r: *const strata_software_renderer) -> u32;
    pub fn strata_software_height(r: *const strata_software_renderer) -> u32;
    pub fn strata_software_read_pixels(r: *const strata_software_renderer, out: *mut u8, capacity: usize) -> c_int;

    pub fn strata_gl_create(ui: *const strata_context, loader: strata_gl_proc_loader) -> *mut strata_gl_renderer;
    pub fn strata_gl_destroy(r: *mut strata_gl_renderer);
    pub fn strata_gl_render(r: *mut strata_gl_renderer, ui: *const strata_context);
    pub fn strata_gl_update_atlas(r: *mut strata_gl_renderer, ui: *const strata_context) -> c_int;
    pub fn strata_gl_set_state_restore(r: *mut strata_gl_renderer, on: c_int);
    pub fn strata_gl_create_texture(
        r: *mut strata_gl_renderer,
        width: u32,
        height: u32,
        rgba: *const u8,
    ) -> u32;
    pub fn strata_gl_destroy_texture(r: *mut strata_gl_renderer, texture_id: u32);
}
