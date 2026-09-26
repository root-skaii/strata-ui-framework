#pragma once

#include "strata/bidi.hpp"
#include "strata/font.hpp"
#include "strata/types.hpp"
#include "strata/vmem.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

// 16 bytes: half of what a naive float2 + float2 + rgba layout costs per corner
struct vertex {
    vec2  pos;
    u16   u{};
    u16   v{};
    color col;
};
static_assert(sizeof(vertex) == 16);

// one rounded rectangle, evaluated analytically per pixel by the shader:
// per-corner radii, angled / radial gradient, inner border and a soft drop shadow in a
// single quad. 80 bytes, identical layout to `struct shape` in ui.hlsl. everything in physical pixels.
struct shape_record {
    vec2               center;
    vec2               half_size;
    std::array<f32, 4> radius;       // tl, tr, br, bl
    color              fill_top;     // gradient start (top for the default direction)
    color              fill_bottom;  // gradient end
    color              border;
    color              shadow;
    f32                border_width;
    f32                shadow_blur;
    vec2               shadow_offset;
    vec2               gradient_dir;  // unit vector the fill runs along; (0, 1) = top to bottom
    f32                gradient_kind; // 0 linear, 1 radial (start at the centre, end at the edge)
    f32                extra;         // backdrop commands: noise amount
};
static_assert(sizeof(shape_record) == 80);

// indices are 16 bit and relative to the command's own `vtx_offset`, which halves index bandwidth. a command
// therefore spans at most `max_command_vertices` vertices; the draw list splits one that would reach past that,
// exactly as it does on a clip or texture change.
using index_t = u16;
inline constexpr u32 max_command_vertices = 1u << 16;

struct draw_cmd {
    rect       clip;              // physical pixels
    u32        idx_offset{};
    u32        idx_count{};
    u32        vtx_offset{};      // what this command's indices are relative to (BaseVertexLocation)
    texture_id texture{};         // 0: glyphs and shapes (font atlas); otherwise the image the whole command draws
    f32        blur{};            // > 0: a backdrop command: the quads show the frame so far, blurred by this many pixels
};

// what a renderer backend consumes. all spans are valid until the next begin().
struct draw_data {
    std::span<const vertex>   vertices;
    std::span<const index_t>  indices;
    std::span<const draw_cmd> commands;
    std::span<const shape_record> shapes;
    vec2                      display_size; // physical pixels
    // identifies these contents, so a renderer can tell that the buffer it already holds is still the right one and
    // skip the upload (context::end_frame computes it; see context::frame_unchanged). 0 = unknown, always upload.
    // it is a hash, so a renderer must treat it as a hint about equality and nothing more.
    u64                       content_hash{};
};

enum class corners : u8 {
    none   = 0,
    tl     = 1,
    tr     = 2,
    br     = 4,
    bl     = 8,
    top    = tl | tr,
    bottom = bl | br,
    all    = tl | tr | br | bl,
};

[[nodiscard]] constexpr corners operator|(corners a, corners b) noexcept
{
    return static_cast<corners>(static_cast<u8>(a) | static_cast<u8>(b));
}

// synthesized text styles (the font atlas has one face per font id): bold is a double strike, italic a slant of the
// glyph quads, underline / strike are lines. none of them changes the advance, so layout is the same as for plain text
enum class text_flags : u8 {
    none      = 0,
    bold      = 1,
    italic    = 2,
    underline = 4,
    strike    = 8,
};

[[nodiscard]] constexpr text_flags operator|(text_flags a, text_flags b) noexcept
{
    return static_cast<text_flags>(static_cast<u8>(a) | static_cast<u8>(b));
}

[[nodiscard]] constexpr bool has_text_flag(text_flags set, text_flags f) noexcept
{
    return (static_cast<u8>(set) & static_cast<u8>(f)) != 0;
}

[[nodiscard]] constexpr bool has_corner(corners set, corners c) noexcept
{
    return (static_cast<u8>(set) & static_cast<u8>(c)) != 0;
}

// description of a rounded rectangle for draw_list::shape(). all colors are
// straight alpha; a fully transparent color disables that part.
struct shape_style {
    std::array<f32, 4> radius{};             // tl, tr, br, bl
    color              fill_top{0, 0, 0, 0}; // start of the gradient
    color              fill_bottom{0, 0, 0, 0}; // end of the gradient
    vec2               gradient_dir{0.0f, 1.0f}; // direction the gradient runs along (any length); default top to bottom
    bool               radial{};             // fill_top in the centre fading to fill_bottom at the edge
    color              border{0, 0, 0, 0};   // drawn inside the edge
    f32                border_width{};
    color              shadow{0, 0, 0, 0};
    f32                shadow_blur{};        // reach of the shadow in pixels
    vec2               shadow_offset{};
};

[[nodiscard]] constexpr std::array<f32, 4> radii(f32 all) noexcept { return {all, all, all, all}; }

[[nodiscard]] constexpr std::array<f32, 4> radii(f32 r, corners which) noexcept
{
    return {has_corner(which, corners::tl) ? r : 0.0f, has_corner(which, corners::tr) ? r : 0.0f,
            has_corner(which, corners::br) ? r : 0.0f, has_corner(which, corners::bl) ? r : 0.0f};
}

// unit direction for a gradient angle in degrees: 0 = left to right, 90 = top to bottom, 45 = towards the bottom right
[[nodiscard]] vec2 gradient_direction(f32 degrees) noexcept;

// reservation sizes are address space only, memory is committed on demand
struct draw_list_limits {
    std::size_t max_vertices = 1u << 20;
    std::size_t max_indices  = 1u << 22;
    std::size_t max_commands = 1u << 14;
    std::size_t max_shapes   = 1u << 17;
};

// all coordinates given to the draw list are logical pixels; with a scale other than 1 (dpi scaling) they are
// multiplied on the way out, so the commands the renderer gets are in physical pixels.
class draw_list {
public:
    explicit draw_list(draw_list_limits limits = {});

    // `display_size` in physical pixels
    void begin(vec2 display_size, const font_atlas& atlas, f32 scale = 1.0f);

    // the four corners of a rectangle only, the way a viewport marks what is selected or what the pointer is over:
    // an outline that reads on top of a busy scene without boxing it in. `arm` is how far each leg runs along its
    // edge (0 = a quarter of the shorter side); it never grows past half of it, so small boxes stay corners.
    void corner_brackets(const rect& r, color c, f32 thickness = 2.0f, f32 arm = 0.0f);

    void push_clip(const rect& r) noexcept;
    void pop_clip() noexcept;
    // replaces the clip instead of intersecting with it (popups escape their window); pop_clip() undoes it
    void push_clip_absolute(const rect& r) noexcept;
    [[nodiscard]] const rect& clip() const noexcept { return clip_; } // logical
    [[nodiscard]] f32 scale() const noexcept { return scale_; }

    // multiplies the alpha of everything drawn until the matching pop_alpha() (fades, page transitions)
    void push_alpha(f32 a) noexcept;
    void pop_alpha() noexcept;
    [[nodiscard]] f32 alpha() const noexcept { return alpha_; }

    // the general primitive: rounded rect with gradient, border and shadow
    void shape(const rect& r, const shape_style& style);

    void rect_filled(const rect& r, color c, f32 rounding = 0, corners which = corners::all);
    void rect_outline(const rect& r, color c, f32 rounding = 0, f32 thickness = 1, corners which = corners::all);
    void rect_gradient_v(const rect& r, color top, color bottom);
    // untextured quad with one color per corner (bilinearly interpolated), square corners
    void rect_gradient(const rect& r, color tl, color tr, color br, color bl);
    // gradient along any direction (see gradient_direction), rounded corners allowed
    void rect_gradient_angle(const rect& r, color from, color to, f32 degrees, f32 rounding = 0);
    // fades from `inner` at the centre to `outer` at the edge of the rect
    void rect_gradient_radial(const rect& r, color inner, color outer, f32 rounding = 0);
    void line(vec2 a, vec2 b, color c, f32 thickness = 1);
    void triangle_filled(vec2 a, vec2 b, vec2 c, color col);
    // convex polygon with an antialiased edge (up to 64 points)
    void polygon_filled(std::span<const vec2> points, color col);
    // the area between a polyline and the horizontal line y = base_y (logical), shaded from `top_col` at the line to
    // `base_col` at the base: area charts. the x of the points must not decrease.
    void area_fill(std::span<const vec2> top, f32 base_y, color top_col, color base_col);
    void text(vec2 pos, color c, std::string_view s, font_id font = 0, text_flags style = text_flags::none);

    // antialiased polyline with mitred joins (butt ends). `thickness` in logical pixels.
    void polyline(std::span<const vec2> points, color c, f32 thickness = 1.0f, bool closed = false);
    // curves, tessellated to a polyline (segments == 0: as many as the length calls for)
    void bezier_cubic(vec2 p0, vec2 p1, vec2 p2, vec2 p3, color c, f32 thickness = 1.0f, u32 segments = 0);
    void bezier_quadratic(vec2 p0, vec2 p1, vec2 p2, color c, f32 thickness = 1.0f, u32 segments = 0);
    // arc from angle a0 to a1 (radians, 0 = +x, growing clockwise on screen)
    void arc(vec2 center, f32 radius, f32 a0, f32 a1, color c, f32 thickness = 1.0f, u32 segments = 0);
    void circle_filled(vec2 center, f32 radius, color c);
    void circle(vec2 center, f32 radius, color c, f32 thickness = 1.0f);

    // a texture created by the renderer, stretched over `r`; [uv0, uv1] is the visible part, `tint` multiplies the
    // texels, `radius` (tl, tr, br, bl) rounds the corners. it is drawn by its own command, so it merges with nothing.
    // an image takes a shape_record: shadow_offset = uv0, (border_width, shadow_blur) = uv1, the rest unused.
    void image(const rect& r, texture_id tex, vec2 uv0 = {0.0f, 0.0f}, vec2 uv1 = {1.0f, 1.0f},
               color tint = {255, 255, 255, 255}, const std::array<f32, 4>& radius = {});
    // frosted glass: a rounded panel showing what was drawn before it, blurred by `blur_radius` logical pixels, re-graded
    // by `saturation` / `brightness` (1 = as is) and covered by `tint` and `noise` grain (0..1). renderers that cannot
    // read their target back fill `tint` alone, so give it some alpha. drawn by its own command.
    void backdrop(const rect& r, f32 blur_radius, color tint, const std::array<f32, 4>& radius = {}, f32 noise = 0.03f,
                  f32 saturation = 1.0f, f32 brightness = 1.0f);

    // draw-order control: reordering whole runs of commands changes what is on top without touching vertex data.
    // a run always starts with break_command().
    struct command_range {
        u32 first{};
        u32 count{};
    };
    [[nodiscard]] u32 command_count() const noexcept { return static_cast<u32>(commands_.size()); }
    void break_command() noexcept { force_new_command_ = true; } // next primitive opens a new command
    // rewrites the command list as the concatenation of `order`: slices that cover every command exactly once
    void reorder_commands(std::span<const command_range> order);

    [[nodiscard]] draw_data data() const noexcept;
    // true if the vertex / index / command reservation ran out this frame
    [[nodiscard]] bool overflowed() const noexcept { return overflow_; }
    // pushes that overran the clip / alpha stack this frame: a nesting deeper than the draw list can hold.
    // the geometry is still correct, but the innermost levels were not clipped / faded.
    [[nodiscard]] u32 clip_stack_overflows() const noexcept { return clip_overflows_; }
    [[nodiscard]] u32 alpha_stack_overflows() const noexcept { return alpha_overflows_; }

private:
    static constexpr u32 max_polygon_points = 64;
    static constexpr u32 max_clip_depth     = 32;
    static constexpr u32 max_alpha_depth    = 16;

    struct prim {
        vertex*  v{};
        index_t* i{};
        u32      base{}; // this primitive's first vertex, relative to the command's vtx_offset
        // an index for the primitive's vertex `offset`, narrowed to the 16-bit index type
        [[nodiscard]] index_t idx(u32 offset) const noexcept { return static_cast<index_t>(base + offset); }
    };

    // `vertex_count` must not exceed max_command_vertices: callers whose size is driven by their input
    // (text, polyline, area_fill) emit in chunks that stay under it.
    [[nodiscard]] bool reserve(u32 vertex_count, u32 index_count, prim& out) noexcept;
    [[nodiscard]] color fade(color c) const noexcept { return alpha_ >= 0.999f ? c : c.scaled_alpha(alpha_); }
    void set_clip(const rect& logical) noexcept;
    void add_quad(const rect& r, color tl, color tr, color br, color bl) noexcept;
    void fill_convex_aa(std::span<const vec2> pts, color c) noexcept; // logical points
    // one quad over `bounds` (logical) whose pixels are shaded from shape record `record_index`; false if the reservation
    // ran out. texture_ / blur_ decide which flavour of command it lands in
    [[nodiscard]] bool emit_quad_for(const rect& bounds, u32 record_index, color vertex_color) noexcept;

    vmem_array<vertex>   vertices_;
    vmem_array<index_t>  indices_;
    vmem_array<draw_cmd> commands_;
    vmem_array<shape_record> shapes_;
    vmem_array<draw_cmd> reorder_scratch_;

    const font_atlas* atlas_{};
    vec2              display_size_{};
    rect              clip_{};      // logical
    rect              phys_clip_{}; // clip_ in physical pixels, what commands carry
    u16               white_u_{};
    u16               white_v_{};
    bool              overflow_{};
    bool              force_new_command_{};
    texture_id        texture_{}; // the texture the primitive being reserved belongs to
    f32               blur_{};    // the blur radius of the backdrop being reserved
    f32               scale_{1.0f};
    f32               alpha_{1.0f};
    std::array<f32, max_alpha_depth> alpha_stack_{};
    u32               alpha_depth_{};
    // pushes past the capacity of either stack are counted rather than stored, so the matching pop skips instead of
    // restoring a value from the wrong level (which used to leave the clip or the alpha wrong for the rest of the
    // frame). the *_dropped_ counters are the live depth a pop consumes; the *_overflows_ ones only ever grow, so a
    // balanced frame still reports that it ran out (see frame_stats).
    u32               alpha_dropped_{};
    u32               clip_dropped_{};
    u32               alpha_overflows_{};
    u32               clip_overflows_{};

    std::array<rect, max_clip_depth> clip_stack_{};
    u32                              clip_depth_{};

    // one underline / strike-through bar: text() collects them while the glyph vertices are still the tail of the
    // arrays and draws them after. a vector (not a fixed array) because a wrapped paragraph drawn in one call has a
    // bar per line and quietly losing the ones past a fixed count is worse than reusing this capacity every frame.
    struct span_line {
        f32 x0{}, x1{}, base{};
    };

    std::vector<vec2>      poly_pts_;    // scratch of polyline()
    std::vector<vec2>      poly_nrm_;
    std::vector<span_line> text_spans_;  // scratch of text()
    std::string            rtl_scratch_; // text(): the run reordered into visual order
    visual_scratch         bidi_scratch_;
};

} // namespace strata
