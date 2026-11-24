//! Safe Rust bindings over `strata-sys` (the C interface to the `strata` immediate-mode UI library).

use std::ffi::{c_char, c_int, c_void, CString};
use std::ptr::{self, NonNull};

pub use strata_sys as sys;

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Vec2 {
    pub x: f32,
    pub y: f32,
}

impl Vec2 {
    pub const fn new(x: f32, y: f32) -> Self {
        Self { x, y }
    }
}

impl From<Vec2> for sys::strata_vec2 {
    fn from(v: Vec2) -> Self {
        Self { x: v.x, y: v.y }
    }
}

impl From<sys::strata_vec2> for Vec2 {
    fn from(v: sys::strata_vec2) -> Self {
        Self { x: v.x, y: v.y }
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Rect {
    pub min_x: f32,
    pub min_y: f32,
    pub max_x: f32,
    pub max_y: f32,
}

impl Rect {
    pub const fn new(min_x: f32, min_y: f32, max_x: f32, max_y: f32) -> Self {
        Self {
            min_x,
            min_y,
            max_x,
            max_y,
        }
    }
}

impl From<Rect> for sys::strata_rect {
    fn from(r: Rect) -> Self {
        Self {
            min_x: r.min_x,
            min_y: r.min_y,
            max_x: r.max_x,
            max_y: r.max_y,
        }
    }
}

impl From<sys::strata_rect> for Rect {
    fn from(r: sys::strata_rect) -> Self {
        Self {
            min_x: r.min_x,
            min_y: r.min_y,
            max_x: r.max_x,
            max_y: r.max_y,
        }
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl Color {
    pub const fn rgba(r: u8, g: u8, b: u8, a: u8) -> Self {
        Self { r, g, b, a }
    }

    pub const fn rgb(r: u8, g: u8, b: u8) -> Self {
        Self { r, g, b, a: 255 }
    }
}

impl From<Color> for sys::strata_color {
    fn from(c: Color) -> Self {
        Self {
            r: c.r,
            g: c.g,
            b: c.b,
            a: c.a,
        }
    }
}

impl From<sys::strata_color> for Color {
    fn from(c: sys::strata_color) -> Self {
        Self {
            r: c.r,
            g: c.g,
            b: c.b,
            a: c.a,
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct Config<'a> {
    pub font_face: Option<&'a str>,
    pub font_size: f32,
    pub theme: Option<&'a str>,
}

#[derive(Debug, Clone)]
pub struct Input {
    pub mouse_pos: Vec2,
    pub mouse_down: [bool; 3],
    pub wheel: f32,
    pub delta_time: f32,
    pub display_size: Vec2,
    pub ctrl: bool,
    pub shift: bool,
    pub alt: bool,
    pub typed: String,
    pub keys: Vec<u32>,
}

impl Default for Input {
    fn default() -> Self {
        Self {
            mouse_pos: Vec2::new(-10000.0, -10000.0),
            mouse_down: [false; 3],
            wheel: 0.0,
            delta_time: 1.0 / 60.0,
            display_size: Vec2::new(1280.0, 720.0),
            ctrl: false,
            shift: false,
            alt: false,
            typed: String::new(),
            keys: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DockZone {
    Center = 0,
    Left = 1,
    Right = 2,
    Top = 3,
    Bottom = 4,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DockSide {
    Left = 0,
    Right = 1,
    Top = 2,
    Bottom = 3,
}

pub mod window_flags {
    pub const NONE: u32 = 0;
    pub const RESIZABLE: u32 = strata_sys::STRATA_WINDOW_RESIZABLE;
    pub const NO_TITLE_BAR: u32 = strata_sys::STRATA_WINDOW_NO_TITLE_BAR;
    pub const NO_COLLAPSE: u32 = strata_sys::STRATA_WINDOW_NO_COLLAPSE;
    pub const NO_MOVE: u32 = strata_sys::STRATA_WINDOW_NO_MOVE;
    pub const NO_BACKGROUND: u32 = strata_sys::STRATA_WINDOW_NO_BACKGROUND;
    pub const DOCKABLE: u32 = strata_sys::STRATA_WINDOW_DOCKABLE;
    pub const NO_INPUTS: u32 = strata_sys::STRATA_WINDOW_NO_INPUTS;
    pub const CLICK_THROUGH: u32 = strata_sys::STRATA_WINDOW_CLICK_THROUGH;
}

fn to_cstr(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| {
        let cleaned: Vec<u8> = s.bytes().filter(|&b| b != 0).collect();
        CString::new(cleaned).unwrap()
    })
}

pub struct Context {
    raw: NonNull<sys::strata_context>,
}

impl Context {
    pub fn new(config: &Config<'_>) -> Option<Self> {
        let font_c = config.font_face.map(to_cstr);
        let theme_c = config.theme.map(to_cstr);
        let cfg = sys::strata_config {
            font_face: font_c.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
            font_size: config.font_size,
            theme: theme_c.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
        };
        let ptr = unsafe { sys::strata_context_create(&cfg) };
        NonNull::new(ptr).map(|raw| Self { raw })
    }

    pub fn with_defaults() -> Option<Self> {
        let ptr = unsafe { sys::strata_context_create(ptr::null()) };
        NonNull::new(ptr).map(|raw| Self { raw })
    }

    pub fn as_ptr(&self) -> *const sys::strata_context {
        self.raw.as_ptr()
    }

    pub fn as_mut_ptr(&mut self) -> *mut sys::strata_context {
        self.raw.as_ptr()
    }

    pub fn set_theme(&mut self, name: &str) -> bool {
        let c = to_cstr(name);
        unsafe { sys::strata_set_theme(self.raw.as_ptr(), c.as_ptr()) != 0 }
    }

    pub fn set_scale(&mut self, scale: f32) -> bool {
        unsafe { sys::strata_set_scale(self.raw.as_ptr(), scale) != 0 }
    }

    pub fn scale(&self) -> f32 {
        unsafe { sys::strata_scale(self.raw.as_ptr()) }
    }

    pub fn frame<F: FnOnce(&mut Ui<'_>)>(&mut self, input: &Input, build: F) {
        let mut raw_in = sys::strata_input {
            mouse_pos: input.mouse_pos.into(),
            mouse_down: [
                input.mouse_down[0] as u8,
                input.mouse_down[1] as u8,
                input.mouse_down[2] as u8,
            ],
            wheel: input.wheel,
            delta_time: input.delta_time,
            display_w: input.display_size.x,
            display_h: input.display_size.y,
            ctrl: input.ctrl as u8,
            shift: input.shift as u8,
            alt: input.alt as u8,
            typed_utf8: if input.typed.is_empty() {
                ptr::null()
            } else {
                input.typed.as_ptr() as *const c_char
            },
            typed_len: input.typed.len() as u32,
            keys: [0; sys::STRATA_MAX_KEYS],
            key_count: input.keys.len().min(sys::STRATA_MAX_KEYS) as u32,
        };
        for (dst, &src) in raw_in.keys.iter_mut().zip(input.keys.iter()) {
            *dst = src;
        }
        unsafe { sys::strata_begin_frame(self.raw.as_ptr(), &raw_in) };
        let mut ui = Ui { ctx: self };
        build(&mut ui);
        unsafe { sys::strata_end_frame(self.raw.as_ptr()) };
    }

    pub fn want_capture_mouse(&self) -> bool {
        unsafe { sys::strata_want_capture_mouse(self.raw.as_ptr()) != 0 }
    }

    pub fn want_text_input(&self) -> bool {
        unsafe { sys::strata_want_text_input(self.raw.as_ptr()) != 0 }
    }

    pub fn cursor(&self) -> i32 {
        unsafe { sys::strata_cursor(self.raw.as_ptr()) }
    }

    pub fn set_keyboard_nav(&mut self, enabled: bool) {
        unsafe { sys::strata_set_keyboard_nav(self.raw.as_ptr(), enabled as c_int) };
    }

    pub fn keyboard_nav(&self) -> bool {
        unsafe { sys::strata_keyboard_nav(self.raw.as_ptr()) != 0 }
    }

    pub fn stats(&self) -> sys::strata_frame_stats {
        let mut st = sys::strata_frame_stats::default();
        unsafe { sys::strata_get_frame_stats(self.raw.as_ptr(), &mut st) };
        st
    }

    pub fn draw_data(&self) -> sys::strata_draw_data {
        let mut dd = sys::strata_draw_data::default();
        unsafe { sys::strata_get_draw_data(self.raw.as_ptr(), &mut dd) };
        dd
    }

    pub fn dock_save_layout(&self) -> String {
        let len = unsafe { sys::strata_dock_save_layout(self.raw.as_ptr(), ptr::null_mut(), 0) };
        let mut buf = vec![0u8; len + 1];
        unsafe {
            sys::strata_dock_save_layout(self.raw.as_ptr(), buf.as_mut_ptr() as *mut c_char, buf.len());
        }
        buf.truncate(len);
        String::from_utf8_lossy(&buf).into_owned()
    }

    pub fn dock_load_layout(&mut self, layout: &str) -> bool {
        let c = to_cstr(layout);
        unsafe { sys::strata_dock_load_layout(self.raw.as_ptr(), c.as_ptr()) != 0 }
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        unsafe { sys::strata_context_destroy(self.raw.as_ptr()) };
    }
}

/// Active frame handle passed to the `Context::frame` closure.
pub struct Ui<'a> {
    ctx: &'a mut Context,
}

impl<'a> Ui<'a> {
    fn ptr(&mut self) -> *mut sys::strata_context {
        self.ctx.raw.as_ptr()
    }

    pub fn window<F: FnOnce(&mut Ui<'_>)>(
        &mut self,
        title: &str,
        pos: Vec2,
        size: Vec2,
        flags: u32,
        body: F,
    ) {
        let c = to_cstr(title);
        let open = unsafe {
            sys::strata_begin_window(self.ptr(), c.as_ptr(), pos.x, pos.y, size.x, size.y, flags) != 0
        };
        if open {
            body(self);
        }
        unsafe { sys::strata_end_window(self.ptr()) };
    }

    pub fn dock_area(&mut self, space: Option<&str>, area: Rect) {
        let c = space.map(to_cstr);
        unsafe {
            sys::strata_dock_area(
                self.ptr(),
                c.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                area.into(),
            );
        }
    }

    pub fn dock_edge(&mut self, space: &str, side: DockSide, size: f32, region: Rect) -> Rect {
        let c = to_cstr(space);
        unsafe { sys::strata_dock_edge(self.ptr(), c.as_ptr(), side as c_int, size, region.into()).into() }
    }

    pub fn dock_window(
        &mut self,
        title: &str,
        zone: DockZone,
        target: Option<&str>,
        size: f32,
        space: Option<&str>,
    ) -> bool {
        let t = to_cstr(title);
        let tgt = target.map(to_cstr);
        let sp = space.map(to_cstr);
        unsafe {
            sys::strata_dock_window(
                self.ptr(),
                t.as_ptr(),
                zone as c_int,
                tgt.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                size,
                sp.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
            ) != 0
        }
    }

    pub fn text(&mut self, s: &str) {
        let c = to_cstr(s);
        unsafe { sys::strata_text(self.ptr(), c.as_ptr()) };
    }

    pub fn text_wrapped(&mut self, s: &str) {
        let c = to_cstr(s);
        unsafe { sys::strata_text_wrapped(self.ptr(), c.as_ptr()) };
    }

    pub fn text_dim(&mut self, s: &str) {
        let c = to_cstr(s);
        unsafe { sys::strata_text_dim(self.ptr(), c.as_ptr()) };
    }

    pub fn text_colored(&mut self, color: Color, s: &str) {
        let c = to_cstr(s);
        unsafe { sys::strata_text_colored(self.ptr(), color.into(), c.as_ptr()) };
    }

    pub fn button(&mut self, label: &str) -> bool {
        let c = to_cstr(label);
        unsafe { sys::strata_button(self.ptr(), c.as_ptr()) != 0 }
    }

    pub fn checkbox(&mut self, label: &str, value: &mut bool) -> bool {
        let c = to_cstr(label);
        let mut v = *value as c_int;
        let changed = unsafe { sys::strata_checkbox(self.ptr(), c.as_ptr(), &mut v) != 0 };
        *value = v != 0;
        changed
    }

    pub fn toggle(&mut self, label: &str, value: &mut bool) -> bool {
        let c = to_cstr(label);
        let mut v = *value as c_int;
        let changed = unsafe { sys::strata_toggle(self.ptr(), c.as_ptr(), &mut v) != 0 };
        *value = v != 0;
        changed
    }

    pub fn slider_f32(&mut self, label: &str, value: &mut f32, lo: f32, hi: f32) -> bool {
        let c = to_cstr(label);
        unsafe { sys::strata_slider_float(self.ptr(), c.as_ptr(), value, lo, hi) != 0 }
    }

    pub fn slider_i32(&mut self, label: &str, value: &mut i32, lo: i32, hi: i32) -> bool {
        let c = to_cstr(label);
        let mut v = *value as c_int;
        let changed = unsafe { sys::strata_slider_int(self.ptr(), c.as_ptr(), &mut v, lo, hi) != 0 };
        *value = v;
        changed
    }

    pub fn range_slider(&mut self, label: &str, lo_val: &mut f32, hi_val: &mut f32, lo: f32, hi: f32) -> bool {
        let c = to_cstr(label);
        unsafe { sys::strata_range_slider(self.ptr(), c.as_ptr(), lo_val, hi_val, lo, hi) != 0 }
    }

    pub fn knob(&mut self, label: &str, value: &mut f32, lo: f32, hi: f32, default_val: Option<f32>) -> bool {
        let c = to_cstr(label);
        let def = default_val.unwrap_or(f32::NAN);
        unsafe { sys::strata_knob(self.ptr(), c.as_ptr(), value, lo, hi, def) != 0 }
    }

    pub fn input_text(&mut self, label: &str, text: &mut String, max_bytes: usize) -> bool {
        let c = to_cstr(label);
        let cap = max_bytes.max(text.len() + 1).max(64);
        let mut buf = vec![0u8; cap];
        let copy_n = text.len().min(cap - 1);
        buf[..copy_n].copy_from_slice(&text.as_bytes()[..copy_n]);
        let changed = unsafe {
            sys::strata_input_text(self.ptr(), c.as_ptr(), buf.as_mut_ptr() as *mut c_char, cap) != 0
        };
        if changed {
            let nul = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
            *text = String::from_utf8_lossy(&buf[..nul]).into_owned();
        }
        changed
    }

    pub fn combo(&mut self, label: &str, current: &mut i32, items: &[&str]) -> bool {
        let c = to_cstr(label);
        let c_items: Vec<CString> = items.iter().map(|s| to_cstr(s)).collect();
        let ptrs: Vec<*const c_char> = c_items.iter().map(|s| s.as_ptr()).collect();
        let mut idx = *current as c_int;
        let changed = unsafe {
            sys::strata_combo(self.ptr(), c.as_ptr(), &mut idx, ptrs.as_ptr(), ptrs.len() as c_int) != 0
        };
        *current = idx;
        changed
    }

    pub fn progress_bar(&mut self, fraction: f32, overlay: Option<&str>) {
        let c = overlay.map(to_cstr);
        unsafe {
            sys::strata_progress_bar(
                self.ptr(),
                fraction,
                c.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
            );
        }
    }

    pub fn separator(&mut self) {
        unsafe { sys::strata_separator(self.ptr()) };
    }

    pub fn spacing(&mut self) {
        unsafe { sys::strata_spacing(self.ptr()) };
    }

    pub fn same_line(&mut self) {
        unsafe { sys::strata_same_line(self.ptr()) };
    }

    pub fn plot_lines(&mut self, label: &str, values: &[f32], size: Vec2) {
        let c = to_cstr(label);
        unsafe {
            sys::strata_plot_lines(self.ptr(), c.as_ptr(), values.as_ptr(), values.len(), size.x, size.y);
        }
    }
}

/// CPU rasteriser backed by `strata::software_renderer`.
pub struct SoftwareRenderer {
    raw: NonNull<sys::strata_software_renderer>,
}

impl SoftwareRenderer {
    pub fn new(ctx: &Context) -> Option<Self> {
        let ptr = unsafe { sys::strata_software_create(ctx.as_ptr()) };
        NonNull::new(ptr).map(|raw| Self { raw })
    }

    pub fn resize(&mut self, width: u32, height: u32) {
        unsafe { sys::strata_software_resize(self.raw.as_ptr(), width, height) };
    }

    pub fn clear(&mut self, color: Color) {
        unsafe { sys::strata_software_clear(self.raw.as_ptr(), color.into()) };
    }

    pub fn render(&mut self, ctx: &Context) {
        unsafe { sys::strata_software_render(self.raw.as_ptr(), ctx.as_ptr()) };
    }

    pub fn size(&self) -> (u32, u32) {
        unsafe {
            (
                sys::strata_software_width(self.raw.as_ptr()),
                sys::strata_software_height(self.raw.as_ptr()),
            )
        }
    }

    pub fn read_pixels(&self) -> Vec<u8> {
        let (w, h) = self.size();
        let mut out = vec![0u8; (w as usize) * (h as usize) * 4];
        unsafe {
            sys::strata_software_read_pixels(self.raw.as_ptr(), out.as_mut_ptr(), out.len());
        }
        out
    }
}

impl Drop for SoftwareRenderer {
    fn drop(&mut self) {
        unsafe { sys::strata_software_destroy(self.raw.as_ptr()) };
    }
}

/// OpenGL 3.3+ renderer backed by `strata::gl_renderer`.
pub struct GlRenderer {
    raw: NonNull<sys::strata_gl_renderer>,
}

impl GlRenderer {
    /// Creates an OpenGL renderer on the current 3.3+ core context.
    /// `loader` resolves GL entry points (e.g. `glfwGetProcAddress`); `None` uses WGL on Windows.
    ///
    /// # Safety
    /// A valid OpenGL 3.3+ core context must be current on the calling thread.
    pub unsafe fn new(ctx: &Context, loader: Option<unsafe extern "C" fn(*const c_char) -> *mut c_void>) -> Option<Self> {
        let ptr = sys::strata_gl_create(ctx.as_ptr(), loader);
        NonNull::new(ptr).map(|raw| Self { raw })
    }

    pub fn render(&mut self, ctx: &Context) {
        unsafe { sys::strata_gl_render(self.raw.as_ptr(), ctx.as_ptr()) };
    }

    pub fn update_atlas(&mut self, ctx: &Context) -> bool {
        unsafe { sys::strata_gl_update_atlas(self.raw.as_ptr(), ctx.as_ptr()) != 0 }
    }

    pub fn set_state_restore(&mut self, on: bool) {
        unsafe { sys::strata_gl_set_state_restore(self.raw.as_ptr(), on as c_int) };
    }

    pub fn create_texture(&mut self, width: u32, height: u32, rgba: &[u8]) -> u32 {
        assert_eq!(rgba.len(), (width as usize) * (height as usize) * 4);
        unsafe { sys::strata_gl_create_texture(self.raw.as_ptr(), width, height, rgba.as_ptr()) }
    }

    pub fn destroy_texture(&mut self, texture_id: u32) {
        unsafe { sys::strata_gl_destroy_texture(self.raw.as_ptr(), texture_id) };
    }
}

impl Drop for GlRenderer {
    fn drop(&mut self) {
        unsafe { sys::strata_gl_destroy(self.raw.as_ptr()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn software_frame_renders_pixels() {
        let mut ctx = Context::with_defaults().expect("create strata context");
        let mut sw = SoftwareRenderer::new(&ctx).expect("create software renderer");
        sw.resize(640, 360);

        let mut checked = true;
        let mut level = 0.4f32;
        let mut rlo = 0.2f32;
        let mut rhi = 0.8f32;
        let mut gain = 0.5f32;
        let input = Input {
            display_size: Vec2::new(640.0, 360.0),
            ..Input::default()
        };

        for _ in 0..2 {
            ctx.frame(&input, |ui| {
                ui.window("rust", Vec2::new(20.0, 20.0), Vec2::new(300.0, 240.0), window_flags::NONE, |ui| {
                    ui.text("hello from rust");
                    ui.checkbox("enabled", &mut checked);
                    ui.slider_f32("level", &mut level, 0.0, 1.0);
                    ui.range_slider("range", &mut rlo, &mut rhi, 0.0, 1.0);
                    ui.knob("gain", &mut gain, 0.0, 1.0, Some(0.5));
                });
            });
        }

        sw.clear(Color::rgb(12, 14, 18));
        sw.render(&ctx);
        let px = sw.read_pixels();
        assert_eq!(px.len(), 640 * 360 * 4);
        let changed = px
            .chunks_exact(4)
            .filter(|c| c[0] != 12 || c[1] != 14 || c[2] != 18)
            .count();
        assert!(changed > 2000, "expected UI pixels, got {changed}");
        assert!(ctx.stats().draw_calls > 0);
    }
}
