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

// 16 bytes: half a naive float2 + float2 + rgba layout
struct vertex {
    vec2  pos;
    u16   u{};
    u16   v{};
    color col;
};
static_assert(sizeof(vertex) == 16);

// one rounded rectangle shaded analytically per pixel: per-corner radii, angled / radial gradient, inner border
// and soft shadow in a single quad. 80 bytes, same layout as `struct shape` in ui.hlsl. physical pixels.
struct shape_record {
    vec2               center;
    vec2               half_size;
    std::array<f32, 4> radius;       // tl, tr, br, bl
    color              fill_top;     // gradient start (top by default)
    color              fill_bottom;  // gradient end
    color              border;
    color              shadow;
    f32                border_width;
    f32                shadow_blur;
    vec2               shadow_offset;
    vec2               gradient_dir;  // unit fill direction; (0, 1) = top to bottom
    f32                gradient_kind; // 0 linear, 1 radial (centre to edge)
    f32                extra;         // backdrop commands: noise amount
};
static_assert(sizeof(shape_record) == 80);

// 16-bit indices relative to the command's `vtx_offset`, so a command spans at most `max_command_vertices`
// vertices; longer runs are split like on a clip or texture change.
using index_t = u16;
inline constexpr u32 max_command_vertices = 1u << 16;

struct draw_cmd {
    rect       clip;              // physical pixels
    u32        idx_offset{};
    u32        idx_count{};
    u32        vtx_offset{};      // base for this command's indices (BaseVertexLocation)
    texture_id texture{};         // 0: font atlas (glyphs, shapes); else the command's image
    f32        blur{};            // > 0: backdrop command showing the frame so far, blurred this much
};

// what a renderer consumes; spans are valid until the next begin().
struct draw_data {
    std::span<const vertex>   vertices;
    std::span<const index_t>  indices;
    std::span<const draw_cmd> commands;
    std::span<const shape_record> shapes;
    vec2                      display_size; // physical pixels
    // content hash so a renderer can skip re-uploading unchanged buffers (set by context::end_frame).
    // 0 = unknown, always upload. a hint only.
    u64                       content_hash{};
    // glyph edge thickening for light text (style::text_contrast), applied by the shader
    f32                       text_contrast{};
};

// how a renderer encodes colours into its target. ui colours are srgb and written as is unless the target differs
enum class output_space : u8 {
    srgb,      // default: 8 / 10-bit UNORM target shown as srgb
    srgb_view, // *_UNORM_SRGB view: encodes on write, so the shader writes linear (and blends in linear light)
    scrgb,     // FP16 scRGB (linear, 1.0 = 80 nits): Windows HDR / advanced colour
    hdr10,     // 10-bit HDR10 (BT.2020, PQ): a game's HDR swap chain
};

struct output_desc {
    output_space space{output_space::srgb};
    // hdr only: ui white in nits. Windows' "SDR content brightness" is the natural value;
    // 200 sits well beside game highlights
    f32 paper_white_nits{200.0f};
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

// synthesized styles (one face per font id): bold = double strike, italic = slanted quads, underline / strike = lines.
// advances are unchanged, so layout matches plain text
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

// a rounded rectangle for draw_list::shape(). straight alpha; a transparent color disables that part.
struct shape_style {
    std::array<f32, 4> radius{};             // tl, tr, br, bl
    color              fill_top{0, 0, 0, 0}; // start of the gradient
    color              fill_bottom{0, 0, 0, 0}; // end of the gradient
    vec2               gradient_dir{0.0f, 1.0f}; // gradient direction (any length); default top to bottom
    bool               radial{};             // fill_top at the centre to fill_bottom at the edge
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

// unit direction for a gradient angle in degrees: 0 = left to right, 90 = top to bottom
[[nodiscard]] vec2 gradient_direction(f32 degrees) noexcept;

// reservations are address space only; memory is committed on demand
struct draw_list_limits {
    std::size_t max_vertices = 1u << 20;
    std::size_t max_indices  = 1u << 22;
    std::size_t max_commands = 1u << 14;
    std::size_t max_shapes   = 1u << 17;
};

// input coordinates are logical pixels, scaled to physical on output.
class draw_list {
public:
    explicit draw_list(draw_list_limits limits = {});

    // `display_size` is physical
    void begin(vec2 display_size, const font_atlas& atlas, f32 scale = 1.0f);

    // corner brackets only (selection / hover in a viewport). `arm` = leg length (0 = a quarter of the shorter side),
    // capped at half of it
    void corner_brackets(const rect& r, color c, f32 thickness = 2.0f, f32 arm = 0.0f);

    void push_clip(const rect& r) noexcept;
    void pop_clip() noexcept;
    // replaces the clip instead of intersecting (popups escape their window); pop_clip() undoes it
    void push_clip_absolute(const rect& r) noexcept;
    [[nodiscard]] const rect& clip() const noexcept { return clip_; } // logical
    [[nodiscard]] f32 scale() const noexcept { return scale_; }

    // multiplies alpha until the matching pop_alpha()
    void push_alpha(f32 a) noexcept;
    void pop_alpha() noexcept;
    [[nodiscard]] f32 alpha() const noexcept { return alpha_; }

    // general primitive: rounded rect with gradient, border and shadow
    void shape(const rect& r, const shape_style& style);

    void rect_filled(const rect& r, color c, f32 rounding = 0, corners which = corners::all);
    void rect_outline(const rect& r, color c, f32 rounding = 0, f32 thickness = 1, corners which = corners::all);
    void rect_gradient_v(const rect& r, color top, color bottom);
    // untextured quad, one bilinearly interpolated color per corner, square corners
    void rect_gradient(const rect& r, color tl, color tr, color br, color bl);
    // gradient in any direction (see gradient_direction), rounded corners allowed
    void rect_gradient_angle(const rect& r, color from, color to, f32 degrees, f32 rounding = 0);
    // `inner` at the centre fading to `outer` at the edge
    void rect_gradient_radial(const rect& r, color inner, color outer, f32 rounding = 0);
    void line(vec2 a, vec2 b, color c, f32 thickness = 1);
    void triangle_filled(vec2 a, vec2 b, vec2 c, color col);
    // convex polygon, antialiased edge, up to 64 points
    void polygon_filled(std::span<const vec2> points, color col);
    // area between a polyline and y = base_y (logical), shaded `top_col` to `base_col`: area charts.
    // x must not decrease.
    void area_fill(std::span<const vec2> top, f32 base_y, color top_col, color base_col);
    void text(vec2 pos, color c, std::string_view s, font_id font = 0, text_flags style = text_flags::none);

    // antialiased polyline, mitred joins, butt ends. `thickness` is logical.
    void polyline(std::span<const vec2> points, color c, f32 thickness = 1.0f, bool closed = false);
    // tessellated to a polyline (segments == 0: chosen by length)
    void bezier_cubic(vec2 p0, vec2 p1, vec2 p2, vec2 p3, color c, f32 thickness = 1.0f, u32 segments = 0);
    void bezier_quadratic(vec2 p0, vec2 p1, vec2 p2, color c, f32 thickness = 1.0f, u32 segments = 0);
    // arc from a0 to a1 (radians, 0 = +x, clockwise on screen)
    void arc(vec2 center, f32 radius, f32 a0, f32 a1, color c, f32 thickness = 1.0f, u32 segments = 0);
    void circle_filled(vec2 center, f32 radius, color c);
    void circle(vec2 center, f32 radius, color c, f32 thickness = 1.0f);

    // renderer texture over `r`: [uv0, uv1] is the visible part, `tint` multiplies, `radius` (tl, tr, br, bl) rounds.
    // own command, merges with nothing. uses a shape_record: shadow_offset = uv0, (border_width, shadow_blur) = uv1.
    void image(const rect& r, texture_id tex, vec2 uv0 = {0.0f, 0.0f}, vec2 uv1 = {1.0f, 1.0f},
               color tint = {255, 255, 255, 255}, const std::array<f32, 4>& radius = {});
    // frosted glass: the content behind blurred by `blur_radius` (logical), graded by `saturation` / `brightness`
    // (1 = as is), covered by `tint` and `noise` (0..1). renderers that cannot read back fill `tint` only, so give it
    // alpha. own command.
    void backdrop(const rect& r, f32 blur_radius, color tint, const std::array<f32, 4>& radius = {}, f32 noise = 0.03f,
                  f32 saturation = 1.0f, f32 brightness = 1.0f);

    // draw order: reordering whole command runs restacks without touching vertices. a run starts with break_command().
    struct command_range {
        u32 first{};
        u32 count{};
    };
    [[nodiscard]] u32 command_count() const noexcept { return static_cast<u32>(commands_.size()); }
    void break_command() noexcept { force_new_command_ = true; } // next primitive opens a new command
    // rewrites the command list as `order`: slices covering every command exactly once
    void reorder_commands(std::span<const command_range> order);

    [[nodiscard]] draw_data data() const noexcept;
    // vertex / index / command reservation ran out this frame
    [[nodiscard]] bool overflowed() const noexcept { return overflow_; }
    // pushes past the clip / alpha stack this frame: the innermost levels were not clipped / faded.
    [[nodiscard]] u32 clip_stack_overflows() const noexcept { return clip_overflows_; }
    [[nodiscard]] u32 alpha_stack_overflows() const noexcept { return alpha_overflows_; }

private:
    static constexpr u32 max_polygon_points = 64;
    static constexpr u32 max_clip_depth     = 32;
    static constexpr u32 max_alpha_depth    = 16;

    struct prim {
        vertex*  v{};
        index_t* i{};
        u32      base{}; // first vertex of this primitive, relative to vtx_offset
        // 16-bit index for the primitive's vertex `offset`
        [[nodiscard]] index_t idx(u32 offset) const noexcept { return static_cast<index_t>(base + offset); }
    };

    // `vertex_count` <= max_command_vertices; input-sized callers (text, polyline, area_fill) emit in chunks.
    [[nodiscard]] bool reserve(u32 vertex_count, u32 index_count, prim& out) noexcept;
    [[nodiscard]] color fade(color c) const noexcept { return alpha_ >= 0.999f ? c : c.scaled_alpha(alpha_); }
    void set_clip(const rect& logical) noexcept;
    void add_quad(const rect& r, color tl, color tr, color br, color bl) noexcept;
    void fill_convex_aa(std::span<const vec2> pts, color c) noexcept; // logical points
    // one quad over `bounds` (logical) shaded from shape record `record_index`; false if out of space.
    // texture_ / blur_ pick the command flavour
    [[nodiscard]] bool emit_quad_for(const rect& bounds, u32 record_index, color vertex_color) noexcept;

    vmem_array<vertex>   vertices_;
    vmem_array<index_t>  indices_;
    vmem_array<draw_cmd> commands_;
    vmem_array<shape_record> shapes_;
    vmem_array<draw_cmd> reorder_scratch_;

    const font_atlas* atlas_{};
    vec2              display_size_{};
    rect              clip_{};      // logical
    rect              phys_clip_{}; // physical pixels, as commands carry it
    u16               white_u_{};
    u16               white_v_{};
    bool              overflow_{};
    bool              force_new_command_{};
    texture_id        texture_{}; // texture of the primitive being reserved
    f32               blur_{};    // blur radius of the backdrop being reserved
    f32               scale_{1.0f};
    f32               alpha_{1.0f};
    std::array<f32, max_alpha_depth> alpha_stack_{};
    u32               alpha_depth_{};
    // pushes past capacity are counted, not stored, so the matching pop skips instead of restoring the wrong level.
    // *_dropped_ is the live excess depth; *_overflows_ only grows, so a balanced frame still reports it.
    u32               alpha_dropped_{};
    u32               clip_dropped_{};
    u32               alpha_overflows_{};
    u32               clip_overflows_{};

    std::array<rect, max_clip_depth> clip_stack_{};
    u32                              clip_depth_{};

    // underline / strike bars, drawn after the glyphs. a vector because a wrapped paragraph has one per line.
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
