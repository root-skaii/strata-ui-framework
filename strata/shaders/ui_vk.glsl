#version 450

// Vulkan shaders matching ui.hlsl (compiled to SPIR-V once per STAGE_* macro).
//   set 0 binding 0  per-frame SSBO of rounded-rect shapes (std430, matches shape_record)
//   set 0 binding 1  font atlas (r8 coverage, linear-clamp)
//   set 1 binding 0  user texture (STAGE_PS_IMAGE) or blurred frame (STAGE_PS_BACKDROP)
//   blur passes use set 0 binding 0 for blur_src and 80-byte push constants (bp0..bp4).
// output is premultiplied alpha: blend = ONE, ONE_MINUS_SRC_ALPHA.

const uint NO_SHAPE = 0xffffffffu;

#if defined(STAGE_VS_MAIN)

layout(push_constant) uniform ui_push {
    vec2 scale;
    vec2 translate;
    vec4 output_cfg;
    vec4 view;
} pc;

layout(location = 0) in  vec2  a_pos;
layout(location = 1) in  uvec2 a_uv;
layout(location = 2) in  vec4  a_col;

layout(location = 0) out vec2      v_uv;
layout(location = 1) out vec4      v_col;
layout(location = 2) flat out uint v_shape_index;

void main()
{
    vec2 ndc = a_pos * pc.scale + pc.translate;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0); // Vulkan clip Y points down
    v_col = a_col;
    if ((a_uv.y & 0x8000u) != 0u) {
        v_uv          = vec2(0.0);
        v_shape_index = a_uv.x | ((a_uv.y & 0x7fffu) << 16u);
    } else {
        v_uv          = vec2(a_uv) * (1.0 / 32767.0);
        v_shape_index = NO_SHAPE;
    }
}

#elif defined(STAGE_VS_FULLSCREEN)

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

#elif defined(STAGE_PS_BLUR_DOWN) || defined(STAGE_PS_BLUR_GAUSS)

layout(push_constant) uniform blur_push {
    vec4 bp0; // xy: destination size in pixels, zw: source-region uv scale
    vec4 bp1; // xy: tap step in source uv, zw: max valid source uv
    vec4 bp2; // x: downsample tap offset (source px), z: gaussian centre weight
    vec4 bp3; // gaussian: weight, offset of the first two pair-taps
    vec4 bp4; // xy: ... and of the third
} bp;

layout(set = 0, binding = 0) uniform sampler2D u_blur_src;
layout(location = 0) out vec4 o_color;

void main()
{
    vec2 uv = gl_FragCoord.xy / bp.bp0.xy * bp.bp0.zw;
#if defined(STAGE_PS_BLUR_DOWN)
    vec2 o = bp.bp1.xy * bp.bp2.x;
    vec4 c = textureLod(u_blur_src, clamp(uv + vec2(-o.x, -o.y), vec2(0.0), bp.bp1.zw), 0.0)
           + textureLod(u_blur_src, clamp(uv + vec2( o.x, -o.y), vec2(0.0), bp.bp1.zw), 0.0)
           + textureLod(u_blur_src, clamp(uv + vec2(-o.x,  o.y), vec2(0.0), bp.bp1.zw), 0.0)
           + textureLod(u_blur_src, clamp(uv + vec2( o.x,  o.y), vec2(0.0), bp.bp1.zw), 0.0);
    o_color = c * 0.25;
#else
    vec4 sum = textureLod(u_blur_src, uv, 0.0) * bp.bp2.z;
    vec2 w   = bp.bp3.xz;
    vec2 o   = bp.bp3.yw;
    sum += (textureLod(u_blur_src, clamp(uv + bp.bp1.xy * o.x, vec2(0.0), bp.bp1.zw), 0.0) +
            textureLod(u_blur_src, clamp(uv - bp.bp1.xy * o.x, vec2(0.0), bp.bp1.zw), 0.0)) * w.x;
    sum += (textureLod(u_blur_src, clamp(uv + bp.bp1.xy * o.y, vec2(0.0), bp.bp1.zw), 0.0) +
            textureLod(u_blur_src, clamp(uv - bp.bp1.xy * o.y, vec2(0.0), bp.bp1.zw), 0.0)) * w.y;
    sum += (textureLod(u_blur_src, clamp(uv + bp.bp1.xy * bp.bp4.y, vec2(0.0), bp.bp1.zw), 0.0) +
            textureLod(u_blur_src, clamp(uv - bp.bp1.xy * bp.bp4.y, vec2(0.0), bp.bp1.zw), 0.0)) * bp.bp4.x;
    o_color = sum;
#endif
}

#else // STAGE_PS_MAIN, STAGE_PS_IMAGE, STAGE_PS_BACKDROP

struct shape {
    vec2  center;
    vec2  half_size;
    vec4  radius;        // tl, tr, br, bl
    uint  fill_top;      // rgba8, r in the low byte
    uint  fill_bottom;
    uint  border_color;
    uint  shadow_color;
    float border_width;
    float shadow_blur;
    vec2  shadow_offset;
    vec2  gradient_dir;
    float gradient_kind; // 0 linear, 1 radial
    float extra;         // backdrop: noise (shadow_offset = saturation, brightness)
};

layout(push_constant) uniform ui_push {
    vec2 scale;
    vec2 translate;
    vec4 output_cfg;
    vec4 view;
    vec4 bp0;
    vec4 bp1;
    vec4 bp2;
    vec4 bp3;
    vec4 bp4;
} pc;

layout(std430, set = 0, binding = 0) readonly buffer shape_table {
    shape shapes[];
};
layout(set = 0, binding = 1) uniform sampler2D u_atlas;
#if defined(STAGE_PS_IMAGE) || defined(STAGE_PS_BACKDROP)
layout(set = 1, binding = 0) uniform sampler2D u_tex;
#endif

layout(location = 0) in vec2      v_uv;
layout(location = 1) in vec4      v_col;
layout(location = 2) flat in uint v_shape_index;

layout(location = 0) out vec4 o_color;

vec4 unpack_color(uint c)
{
    return vec4(c & 255u, (c >> 8u) & 255u, (c >> 16u) & 255u, c >> 24u) * (1.0 / 255.0);
}

vec3 srgb_to_linear(vec3 c)
{
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}

vec3 pq_encode(vec3 l)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 p = pow(clamp(l, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

vec3 encode_rgb(vec3 c)
{
    if (pc.output_cfg.x < 0.5) { return c; }
    vec3 l = srgb_to_linear(clamp(c, 0.0, 1.0));
    if (pc.output_cfg.x < 1.5) { return l; }
    if (pc.output_cfg.x < 2.5) { return l * pc.output_cfg.y; }
    vec3 bt2020 = vec3(
        dot(vec3(0.6274040, 0.3292820, 0.0433136), l),
        dot(vec3(0.0690970, 0.9195400, 0.0113612), l),
        dot(vec3(0.0163916, 0.0880132, 0.8955950), l)
    );
    return pq_encode(bt2020 * pc.output_cfg.y);
}

vec4 encode_output(vec4 premul)
{
    if (pc.output_cfg.x < 0.5 || premul.a <= 0.0) { return premul; }
    return vec4(encode_rgb(premul.rgb / premul.a) * premul.a, premul.a);
}

float sd_round_box(vec2 p, vec2 half_size, vec4 r)
{
    float rr = p.x > 0.0 ? (p.y > 0.0 ? r.z : r.y) : (p.y > 0.0 ? r.w : r.x);
    vec2  q  = abs(p) - half_size + rr;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr;
}

vec4 shade_shape(shape s, vec2 pixel)
{
    vec2  p     = pixel - s.center;
    float d     = sd_round_box(p, s.half_size, s.radius);
    float outer = clamp(0.5 - d, 0.0, 1.0);
    float inner = s.border_width > 0.0 ? clamp(0.5 - (d + s.border_width), 0.0, 1.0) : outer;

    vec4 fill = unpack_color(s.fill_top);
    if (s.fill_top != s.fill_bottom) {
        float extent = max(abs(s.gradient_dir.x) * 2.0 * s.half_size.x + abs(s.gradient_dir.y) * 2.0 * s.half_size.y, 1.0e-3);
        float t      = s.gradient_kind > 0.5 ? clamp(length(p / max(s.half_size, vec2(1.0e-3))), 0.0, 1.0)
                                             : clamp(dot(p, s.gradient_dir) / extent + 0.5, 0.0, 1.0);
        fill = mix(fill, unpack_color(s.fill_bottom), t);
    }

    vec4 result = vec4(fill.rgb * fill.a, fill.a) * inner;
    if (s.border_width > 0.0) {
        vec4 edge = unpack_color(s.border_color);
        result += vec4(edge.rgb * edge.a, edge.a) * max(outer - inner, 0.0);
    }

    vec4 sh = unpack_color(s.shadow_color);
    if (sh.a > 0.0 && s.shadow_blur > 0.0) {
        float ds = sd_round_box(p - s.shadow_offset, s.half_size, s.radius);
        float k  = 1.0 - smoothstep(-s.shadow_blur, s.shadow_blur, ds);
        float sa = sh.a * k * (1.0 - outer);
        result += vec4(sh.rgb * sa, sa) * (1.0 - result.a);
    }
    return result;
}

void main()
{
#if defined(STAGE_PS_MAIN)
    if (v_shape_index != NO_SHAPE) {
        o_color = encode_output(shade_shape(shapes[v_shape_index], gl_FragCoord.xy + pc.view.xy));
        return;
    }
    float coverage = textureLod(u_atlas, v_uv, 0.0).r;
    if (pc.output_cfg.z < 0.0) {
        float w  = max(fwidth(coverage), 0.02);
        coverage = smoothstep(0.5 - w, 0.5 + w, coverage);
    } else if (pc.output_cfg.z > 0.0) {
        float k  = pc.output_cfg.z * dot(v_col.rgb, vec3(0.2126, 0.7152, 0.0722));
        coverage = coverage * (1.0 + k) / (1.0 + k * coverage);
    }
    float a = v_col.a * coverage;
    o_color = encode_output(vec4(v_col.rgb * a, a));
#elif defined(STAGE_PS_IMAGE)
    shape s   = shapes[v_shape_index];
    vec2  p   = gl_FragCoord.xy + pc.view.xy - s.center;
    float cov = clamp(0.5 - sd_round_box(p, s.half_size, s.radius), 0.0, 1.0);

    vec2 t   = clamp(p / (2.0 * s.half_size) + 0.5, 0.0, 1.0);
    vec2 uv0 = s.shadow_offset;
    vec2 uv1 = vec2(s.border_width, s.shadow_blur);
    vec2 uv  = mix(uv0, uv1, t);

    vec2  dim = vec2(textureSize(u_tex, 0));
    vec2  texels_per_pixel = abs(uv1 - uv0) * dim / max(2.0 * s.half_size, vec2(1.0));
    float lod = max(log2(max(max(texels_per_pixel.x, texels_per_pixel.y), 1.0e-4)), 0.0);
    vec4  c   = textureLod(u_tex, uv, lod) * v_col;

    float a = c.a * cov;
    o_color = encode_output(vec4(c.rgb * a, a));
#elif defined(STAGE_PS_BACKDROP)
    shape s   = shapes[v_shape_index];
    vec2  p   = gl_FragCoord.xy + pc.view.xy - s.center;
    float cov = clamp(0.5 - sd_round_box(p, s.half_size, s.radius), 0.0, 1.0);

    vec2  uv   = gl_FragCoord.xy / pc.bp0.xy * pc.bp0.zw;
    vec3  bg   = textureLod(u_tex, uv, 0.0).rgb;
    float lum  = dot(bg, vec3(0.2126, 0.7152, 0.0722));
    bg = mix(vec3(lum), bg, s.shadow_offset.x) * s.shadow_offset.y;
    bg = pc.output_cfg.x < 1.5 ? clamp(bg, 0.0, 1.0) : max(bg, vec3(0.0));
    vec4 tint = unpack_color(s.fill_top);
    vec3 rgb  = mix(bg, encode_rgb(tint.rgb), tint.a);

    float grain = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    rgb = max(rgb + grain * s.extra * 0.5, vec3(0.0));
    o_color = vec4(rgb * cov, cov);
#endif
}

#endif
