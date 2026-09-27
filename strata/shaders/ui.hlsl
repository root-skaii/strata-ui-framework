// shared by the d3d11 and d3d12 backends (shader model 5.0 / dxbc).
//   b0  scale.xy, translate.xy, then the output: encoding, white level, text contrast   (d3d12: root constants)
//   t0  font atlas, r8 coverage
//   t1  per-frame table of rounded-rect shapes (d3d12: root srv)
//   t2  user texture of the draw command, rgba8 straight alpha (only read by ps_image)
//   t3  blur source: the blurred frame (ps_backdrop) or the texture a blur pass reads (ps_blur_*)
//   b1  blur / backdrop parameters (d3d12: root constants)
// output is premultiplied alpha: blend = ONE, INV_SRC_ALPHA.
// ps_main draws every command without a texture (glyphs, shapes); ps_image draws the commands that carry one;
// ps_backdrop draws frosted-glass panels from the blurred frame; vs_fullscreen + ps_blur_down / ps_blur_gauss make it.

cbuffer transform : register(b0)
{
    float2 scale;
    float2 translate;
    // x: output_space: 0 as is (srgb target), 1 linear (*_SRGB view encodes), 2 scRGB (linear, 1.0 = 80 nits),
    //    3 HDR10 (BT.2020, PQ). y: white level (2: nits / 80, 3: nits / 10000). z: text contrast (0 = none)
    float4 output;
};

struct shape
{
    float2 center;
    float2 half_size;
    float4 radius;        // tl, tr, br, bl
    uint   fill_top;      // rgba8, r in the low byte
    uint   fill_bottom;
    uint   border_color;
    uint   shadow_color;
    float  border_width;
    float  shadow_blur;
    float2 shadow_offset;
    float2 gradient_dir;  // unit vector the fill runs along
    float  gradient_kind; // 0 linear, 1 radial
    float  extra;         // backdrop: noise (shadow_offset = saturation, brightness)
};

Texture2D              atlas     : register(t0);
StructuredBuffer<shape> shapes   : register(t1);
Texture2D              image_tex : register(t2);
Texture2D              blur_src  : register(t3);
SamplerState           sampler0  : register(s0);

cbuffer blur_params : register(b1)
{
    float4 bp0; // xy: destination size in pixels (backdrop: display), zw: source-region uv scale
    float4 bp1; // xy: tap step in source uv, zw: max valid source uv
    float4 bp2; // x: downsample tap offset (source px), z: gaussian centre weight
    float4 bp3; // gaussian: weight, offset of the first two pair-taps
    float4 bp4; // xy: ... and of the third
};

static const uint no_shape = 0xffffffffu;

struct vs_input
{
    float2 pos : POSITION;
    uint2  uv  : TEXCOORD0;   // atlas uv * 32767, or (shape index) when bit 15 of .y is set
    float4 col : COLOR0;
};

struct ps_input
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
    nointerpolation uint shape_index : TEXCOORD1;
};

ps_input vs_main(vs_input i)
{
    ps_input o;
    o.pos = float4(i.pos * scale + translate, 0.0f, 1.0f);
    o.col = i.col;
    if (i.uv.y & 0x8000u)
    {
        o.uv          = 0.0f;
        o.shape_index = i.uv.x | ((i.uv.y & 0x7fffu) << 16);
    }
    else
    {
        o.uv          = float2(i.uv) * (1.0f / 32767.0f);
        o.shape_index = no_shape;
    }
    return o;
}

float4 unpack_color(uint c)
{
    return float4(c & 255u, (c >> 8) & 255u, (c >> 16) & 255u, c >> 24) * (1.0f / 255.0f);
}

// ---- output encoding: ui colours are srgb; srgb views, scRGB and HDR10 targets need conversion ----

float3 srgb_to_linear(float3 c)
{
    return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
}

float3 pq_encode(float3 l) // l: linear light, 1 = 10000 nits
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float3 p = pow(saturate(l), m1);
    return pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

float3 encode_rgb(float3 c)
{
    if (output.x < 0.5f) { return c; }
    float3 l = srgb_to_linear(saturate(c));
    if (output.x < 1.5f) { return l; }
    if (output.x < 2.5f) { return l * output.y; }
    const float3x3 bt709_to_bt2020 = float3x3(0.6274040f, 0.3292820f, 0.0433136f,
                                              0.0690970f, 0.9195400f, 0.0113612f,
                                              0.0163916f, 0.0880132f, 0.8955950f);
    return pq_encode(mul(bt709_to_bt2020, l) * output.y);
}

// final step of every pixel shader: premultiplied srgb in, premultiplied target encoding out
float4 encode_output(float4 premul)
{
    if (output.x < 0.5f || premul.a <= 0.0f) { return premul; }
    return float4(encode_rgb(premul.rgb / premul.a) * premul.a, premul.a);
}

// signed distance (pixels) to a box with per-corner radii; y down
float sd_round_box(float2 p, float2 half_size, float4 r)
{
    float rr = p.x > 0.0f ? (p.y > 0.0f ? r.z : r.y) : (p.y > 0.0f ? r.w : r.x);
    float2 q = abs(p) - half_size + rr;
    return min(max(q.x, q.y), 0.0f) + length(max(q, 0.0f)) - rr;
}

float4 shade_shape(shape s, float2 pixel)
{
    float2 p = pixel - s.center;
    float  d = sd_round_box(p, s.half_size, s.radius);

    float outer = saturate(0.5f - d);
    float inner = saturate(0.5f - (d + s.border_width));

    // linear gradient along gradient_dir, or radial from centre to edge
    float  extent = max(abs(s.gradient_dir.x) * 2.0f * s.half_size.x + abs(s.gradient_dir.y) * 2.0f * s.half_size.y, 1.0e-3f);
    float  t      = s.gradient_kind > 0.5f ? saturate(length(p / max(s.half_size, 1.0e-3f)))
                                           : saturate(dot(p, s.gradient_dir) / extent + 0.5f);
    float4 fill = lerp(unpack_color(s.fill_top), unpack_color(s.fill_bottom), t);
    float4 edge = unpack_color(s.border_color);

    float4 result = float4(fill.rgb * fill.a, fill.a) * inner
                  + float4(edge.rgb * edge.a, edge.a) * max(outer - inner, 0.0f);

    float4 sh = unpack_color(s.shadow_color);
    if (sh.a > 0.0f && s.shadow_blur > 0.0f)
    {
        float ds = sd_round_box(p - s.shadow_offset, s.half_size, s.radius);
        float k  = 1.0f - smoothstep(-s.shadow_blur, s.shadow_blur, ds);
        float sa = sh.a * k * (1.0f - outer);   // never under the shape itself
        result += float4(sh.rgb * sa, sa) * (1.0f - result.a);
    }
    return result;
}

float4 ps_main(ps_input i) : SV_Target
{
    if (i.shape_index != no_shape)
    {
        return encode_output(shade_shape(shapes[i.shape_index], i.pos.xy));
    }
    float coverage = atlas.SampleLevel(sampler0, i.uv, 0.0f).r;
    if (output.z > 0.0f)
    {
        // thicken light-on-dark glyph edges in proportion to text lightness (0 and 1 are kept)
        float k  = output.z * dot(i.col.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        coverage = coverage * (1.0f + k) / (1.0f + k * coverage);
    }
    float a = i.col.a * coverage;
    return encode_output(float4(i.col.rgb * a, a));
}

// images reuse shape fields: shadow_offset = top-left uv, (border_width, shadow_blur) = bottom-right uv.
// the rounded box gives coverage.
float4 ps_image(ps_input i) : SV_Target
{
    shape  s   = shapes[i.shape_index];
    float2 p   = i.pos.xy - s.center;
    float  cov = saturate(0.5f - sd_round_box(p, s.half_size, s.radius));

    float2 t    = saturate(p / (2.0f * s.half_size) + 0.5f);
    float2 uv0  = s.shadow_offset;
    float2 uv1  = float2(s.border_width, s.shadow_blur);
    float2 uv   = lerp(uv0, uv1, t);

    // mip level from texels per screen pixel (0 when not minified)
    float2 dim;
    image_tex.GetDimensions(dim.x, dim.y);
    float2 texels_per_pixel = abs(uv1 - uv0) * dim / max(2.0f * s.half_size, 1.0f);
    float  lod = max(log2(max(max(texels_per_pixel.x, texels_per_pixel.y), 1.0e-4f)), 0.0f);
    float4 c   = image_tex.SampleLevel(sampler0, uv, lod) * i.col;

    float a = c.a * cov;
    return encode_output(float4(c.rgb * a, a));
}

// frosted glass: blurred frame in a rounded box, mixed with the tint (fill_top) and grain. opaque where coverage is 1.
float4 ps_backdrop(ps_input i) : SV_Target
{
    shape  s   = shapes[i.shape_index];
    float2 p   = i.pos.xy - s.center;
    float  cov = saturate(0.5f - sd_round_box(p, s.half_size, s.radius));

    float2 uv   = i.pos.xy / bp0.xy * bp0.zw;
    float3 bg   = blur_src.SampleLevel(sampler0, uv, 0.0f).rgb;
    float  lum  = dot(bg, float3(0.2126f, 0.7152f, 0.0722f));
    bg = lerp(float3(lum, lum, lum), bg, s.shadow_offset.x) * s.shadow_offset.y; // saturation, brightness
    bg = output.x < 1.5f ? saturate(bg) : max(bg, 0.0f);                       // (scRGB goes past 1)
    float4 tint = unpack_color(s.fill_top);
    float3 rgb  = lerp(bg, encode_rgb(tint.rgb), tint.a); // (bg is already in target encoding)

    float grain = frac(sin(dot(i.pos.xy, float2(12.9898f, 78.233f))) * 43758.5453f) - 0.5f;
    rgb = max(rgb + grain * s.extra * 0.5f, 0.0f); // (not saturated: scRGB goes past 1)
    return float4(rgb * cov, cov);
}

struct fs_in
{
    float4 pos : SV_POSITION;
};

fs_in vs_fullscreen(uint id : SV_VertexID)
{
    fs_in o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

// box downsample: four bilinear taps cover the pixel's block
float4 ps_blur_down(fs_in i) : SV_Target
{
    float2 uv = i.pos.xy / bp0.xy * bp0.zw;
    float2 o  = bp1.xy * bp2.x;
    float4 c  = blur_src.SampleLevel(sampler0, clamp(uv + float2(-o.x, -o.y), 0.0f, bp1.zw), 0.0f)
              + blur_src.SampleLevel(sampler0, clamp(uv + float2( o.x, -o.y), 0.0f, bp1.zw), 0.0f)
              + blur_src.SampleLevel(sampler0, clamp(uv + float2(-o.x,  o.y), 0.0f, bp1.zw), 0.0f)
              + blur_src.SampleLevel(sampler0, clamp(uv + float2( o.x,  o.y), 0.0f, bp1.zw), 0.0f);
    return c * 0.25f;
}

// one gaussian direction over 13 texels as 7 filtered taps (centre + three pair-taps per side). weights from the cpu
float4 ps_blur_gauss(fs_in i) : SV_Target
{
    float2 uv  = i.pos.xy / bp0.xy * bp0.zw;
    float4 sum = blur_src.SampleLevel(sampler0, uv, 0.0f) * bp2.z;
    float2 w   = bp3.xz;
    float2 o   = bp3.yw;
    sum += (blur_src.SampleLevel(sampler0, clamp(uv + bp1.xy * o.x, 0.0f, bp1.zw), 0.0f) +
            blur_src.SampleLevel(sampler0, clamp(uv - bp1.xy * o.x, 0.0f, bp1.zw), 0.0f)) * w.x;
    sum += (blur_src.SampleLevel(sampler0, clamp(uv + bp1.xy * o.y, 0.0f, bp1.zw), 0.0f) +
            blur_src.SampleLevel(sampler0, clamp(uv - bp1.xy * o.y, 0.0f, bp1.zw), 0.0f)) * w.y;
    sum += (blur_src.SampleLevel(sampler0, clamp(uv + bp1.xy * bp4.y, 0.0f, bp1.zw), 0.0f) +
            blur_src.SampleLevel(sampler0, clamp(uv - bp1.xy * bp4.y, 0.0f, bp1.zw), 0.0f)) * bp4.x;
    return sum;
}
