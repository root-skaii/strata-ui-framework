#include "strata/backend/opengl.hpp"
#include "strata/backend/backend.hpp"

#include "blur_plan.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace strata {

static_assert(renderer_backend<gl_renderer>);

namespace {

// minimal OpenGL 3.3 core types & constants (avoids external loader dependencies)
#ifdef _WIN32
#define STRATA_GLAPI __stdcall
#else
#define STRATA_GLAPI
#endif

using GLenum     = unsigned int;
using GLboolean  = unsigned char;
using GLbitfield = unsigned int;
using GLint      = int;
using GLuint     = unsigned int;
using GLsizei    = int;
using GLfloat    = float;
using GLchar     = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr   = std::ptrdiff_t;

constexpr GLboolean  kGL_FALSE                      = 0;
constexpr GLboolean  kGL_TRUE                       = 1;
constexpr GLenum     kGL_NO_ERROR                   = 0;
constexpr GLenum     kGL_TRIANGLES                  = 0x0004;
constexpr GLenum     kGL_ONE                        = 1;
constexpr GLenum     kGL_ONE_MINUS_SRC_ALPHA        = 0x0303;
constexpr GLenum     kGL_FRONT_AND_BACK             = 0x0408;
constexpr GLenum     kGL_CULL_FACE                  = 0x0B44;
constexpr GLenum     kGL_DEPTH_TEST                 = 0x0B71;
constexpr GLenum     kGL_STENCIL_TEST               = 0x0B90;
constexpr GLenum     kGL_VIEWPORT                   = 0x0BA2;
constexpr GLenum     kGL_BLEND                      = 0x0BE2;
constexpr GLenum     kGL_SCISSOR_BOX                = 0x0C10;
constexpr GLenum     kGL_SCISSOR_TEST               = 0x0C11;
constexpr GLenum     kGL_POLYGON_MODE               = 0x0B40;
constexpr GLenum     kGL_FILL                       = 0x1B02;
constexpr GLenum     kGL_UNPACK_ROW_LENGTH          = 0x0CF2;
constexpr GLenum     kGL_UNPACK_SKIP_ROWS           = 0x0CF3;
constexpr GLenum     kGL_UNPACK_SKIP_PIXELS         = 0x0CF4;
constexpr GLenum     kGL_UNPACK_ALIGNMENT           = 0x0CF5;
constexpr GLenum     kGL_TEXTURE_2D                 = 0x0DE1;
constexpr GLenum     kGL_UNSIGNED_BYTE              = 0x1401;
constexpr GLenum     kGL_UNSIGNED_SHORT             = 0x1403;
constexpr GLenum     kGL_FLOAT                      = 0x1406;
constexpr GLenum     kGL_HALF_FLOAT                 = 0x140B;
constexpr GLenum     kGL_RED                        = 0x1903;
constexpr GLenum     kGL_RGBA                       = 0x1908;
constexpr GLenum     kGL_BGRA                       = 0x80E1;
constexpr GLenum     kGL_NEAREST                    = 0x2600;
constexpr GLenum     kGL_LINEAR                     = 0x2601;
constexpr GLenum     kGL_LINEAR_MIPMAP_LINEAR       = 0x2703;
constexpr GLenum     kGL_TEXTURE_MAG_FILTER         = 0x2800;
constexpr GLenum     kGL_TEXTURE_MIN_FILTER         = 0x2801;
constexpr GLenum     kGL_TEXTURE_WRAP_S             = 0x2802;
constexpr GLenum     kGL_TEXTURE_WRAP_T             = 0x2803;
constexpr GLenum     kGL_CLAMP_TO_EDGE              = 0x812F;
constexpr GLenum     kGL_TEXTURE_BASE_LEVEL         = 0x813C;
constexpr GLenum     kGL_TEXTURE_MAX_LEVEL          = 0x813D;
constexpr GLenum     kGL_RGBA8                      = 0x8058;
constexpr GLenum     kGL_R8                         = 0x8229;
constexpr GLenum     kGL_RGBA16F                    = 0x881A;
constexpr GLenum     kGL_RGBA32UI                   = 0x8D70;
constexpr GLenum     kGL_FUNC_ADD                   = 0x8006;
constexpr GLenum     kGL_BLEND_EQUATION_RGB         = 0x8009;
constexpr GLenum     kGL_BLEND_EQUATION_ALPHA       = 0x883D;
constexpr GLenum     kGL_BLEND_DST_RGB              = 0x80C8;
constexpr GLenum     kGL_BLEND_SRC_RGB              = 0x80C9;
constexpr GLenum     kGL_BLEND_DST_ALPHA            = 0x80CA;
constexpr GLenum     kGL_BLEND_SRC_ALPHA            = 0x80CB;
constexpr GLenum     kGL_TEXTURE0                   = 0x84C0;
constexpr GLenum     kGL_ACTIVE_TEXTURE             = 0x84E0;
constexpr GLenum     kGL_TEXTURE_BINDING_2D         = 0x8069;
constexpr GLenum     kGL_TEXTURE_BUFFER             = 0x8C2A;
constexpr GLenum     kGL_TEXTURE_BINDING_BUFFER     = 0x8C2C;
constexpr GLenum     kGL_SAMPLER_BINDING            = 0x8919;
constexpr GLenum     kGL_ARRAY_BUFFER               = 0x8892;
constexpr GLenum     kGL_ELEMENT_ARRAY_BUFFER       = 0x8893;
constexpr GLenum     kGL_ARRAY_BUFFER_BINDING       = 0x8894;
constexpr GLenum     kGL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895;
constexpr GLenum     kGL_PIXEL_UNPACK_BUFFER        = 0x88EC;
constexpr GLenum     kGL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF;
constexpr GLenum     kGL_STREAM_DRAW                = 0x88E0;
constexpr GLenum     kGL_DYNAMIC_DRAW               = 0x88E8;
constexpr GLenum     kGL_VERTEX_ARRAY_BINDING       = 0x85B5;
constexpr GLenum     kGL_CURRENT_PROGRAM            = 0x8B8D;
constexpr GLenum     kGL_FRAGMENT_SHADER            = 0x8B30;
constexpr GLenum     kGL_VERTEX_SHADER              = 0x8B31;
constexpr GLenum     kGL_COMPILE_STATUS             = 0x8B81;
constexpr GLenum     kGL_LINK_STATUS                = 0x8B82;
constexpr GLenum     kGL_READ_FRAMEBUFFER           = 0x8CA8;
constexpr GLenum     kGL_DRAW_FRAMEBUFFER           = 0x8CA9;
constexpr GLenum     kGL_DRAW_FRAMEBUFFER_BINDING   = 0x8CA6;
constexpr GLenum     kGL_READ_FRAMEBUFFER_BINDING   = 0x8CAA;
constexpr GLenum     kGL_COLOR_ATTACHMENT0          = 0x8CE0;
constexpr GLenum     kGL_FRAMEBUFFER_COMPLETE       = 0x8CD5;
constexpr GLbitfield kGL_COLOR_BUFFER_BIT           = 0x00004000;

struct gl_procs {
    void (STRATA_GLAPI* GetIntegerv)(GLenum, GLint*){};
    GLboolean (STRATA_GLAPI* IsEnabled)(GLenum){};
    void (STRATA_GLAPI* Enable)(GLenum){};
    void (STRATA_GLAPI* Disable)(GLenum){};
    void (STRATA_GLAPI* Viewport)(GLint, GLint, GLsizei, GLsizei){};
    void (STRATA_GLAPI* Scissor)(GLint, GLint, GLsizei, GLsizei){};
    void (STRATA_GLAPI* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum){};
    void (STRATA_GLAPI* BlendEquationSeparate)(GLenum, GLenum){};
    void (STRATA_GLAPI* PolygonMode)(GLenum, GLenum){};
    void (STRATA_GLAPI* PixelStorei)(GLenum, GLint){};
    void (STRATA_GLAPI* ActiveTexture)(GLenum){};
    void (STRATA_GLAPI* GenTextures)(GLsizei, GLuint*){};
    void (STRATA_GLAPI* DeleteTextures)(GLsizei, const GLuint*){};
    void (STRATA_GLAPI* BindTexture)(GLenum, GLuint){};
    void (STRATA_GLAPI* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*){};
    void (STRATA_GLAPI* TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*){};
    void (STRATA_GLAPI* TexParameteri)(GLenum, GLenum, GLint){};
    void (STRATA_GLAPI* TexBuffer)(GLenum, GLenum, GLuint){};
    void (STRATA_GLAPI* GenSamplers)(GLsizei, GLuint*){};
    void (STRATA_GLAPI* DeleteSamplers)(GLsizei, const GLuint*){};
    void (STRATA_GLAPI* BindSampler)(GLuint, GLuint){};
    void (STRATA_GLAPI* SamplerParameteri)(GLuint, GLenum, GLint){};
    void (STRATA_GLAPI* GenBuffers)(GLsizei, GLuint*){};
    void (STRATA_GLAPI* DeleteBuffers)(GLsizei, const GLuint*){};
    void (STRATA_GLAPI* BindBuffer)(GLenum, GLuint){};
    void (STRATA_GLAPI* BufferData)(GLenum, GLsizeiptr, const void*, GLenum){};
    void (STRATA_GLAPI* BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*){};
    void (STRATA_GLAPI* GenVertexArrays)(GLsizei, GLuint*){};
    void (STRATA_GLAPI* DeleteVertexArrays)(GLsizei, const GLuint*){};
    void (STRATA_GLAPI* BindVertexArray)(GLuint){};
    void (STRATA_GLAPI* EnableVertexAttribArray)(GLuint){};
    void (STRATA_GLAPI* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*){};
    void (STRATA_GLAPI* VertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void*){};
    GLuint (STRATA_GLAPI* CreateShader)(GLenum){};
    void (STRATA_GLAPI* DeleteShader)(GLuint){};
    void (STRATA_GLAPI* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*){};
    void (STRATA_GLAPI* CompileShader)(GLuint){};
    void (STRATA_GLAPI* GetShaderiv)(GLuint, GLenum, GLint*){};
    GLuint (STRATA_GLAPI* CreateProgram)(){};
    void (STRATA_GLAPI* DeleteProgram)(GLuint){};
    void (STRATA_GLAPI* AttachShader)(GLuint, GLuint){};
    void (STRATA_GLAPI* LinkProgram)(GLuint){};
    void (STRATA_GLAPI* GetProgramiv)(GLuint, GLenum, GLint*){};
    void (STRATA_GLAPI* UseProgram)(GLuint){};
    GLint (STRATA_GLAPI* GetUniformLocation)(GLuint, const GLchar*){};
    void (STRATA_GLAPI* Uniform1i)(GLint, GLint){};
    void (STRATA_GLAPI* Uniform4fv)(GLint, GLsizei, const GLfloat*){};
    void (STRATA_GLAPI* DrawArrays)(GLenum, GLint, GLsizei){};
    void (STRATA_GLAPI* DrawElementsBaseVertex)(GLenum, GLsizei, GLenum, const void*, GLint){};
    void (STRATA_GLAPI* GenFramebuffers)(GLsizei, GLuint*){};
    void (STRATA_GLAPI* DeleteFramebuffers)(GLsizei, const GLuint*){};
    void (STRATA_GLAPI* BindFramebuffer)(GLenum, GLuint){};
    void (STRATA_GLAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint){};
    GLenum (STRATA_GLAPI* CheckFramebufferStatus)(GLenum){};
    void (STRATA_GLAPI* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum){};
    // optional: GL 4.5 / GL_ARB_robustness
    GLenum (STRATA_GLAPI* GetGraphicsResetStatus)(){};

    [[nodiscard]] bool load(gl_renderer::proc_loader custom) noexcept
    {
#ifdef _WIN32
        HMODULE opengl_mod = ::GetModuleHandleW(L"opengl32.dll");
        if (opengl_mod == nullptr) { opengl_mod = ::LoadLibraryW(L"opengl32.dll"); }
        using wgl_get_proc_fn = PROC(WINAPI*)(LPCSTR);
        const auto wgl_get_proc = opengl_mod != nullptr
                                      ? reinterpret_cast<wgl_get_proc_fn>(reinterpret_cast<void*>(::GetProcAddress(opengl_mod, "wglGetProcAddress")))
                                      : nullptr;
#endif
        const auto resolve = [&](const char* name) -> void* {
            if (custom != nullptr) { return custom(name); }
#ifdef _WIN32
            if (wgl_get_proc != nullptr) {
                void* p = reinterpret_cast<void*>(wgl_get_proc(name));
                const auto v = reinterpret_cast<std::uintptr_t>(p);
                if (v != 0 && v != 1 && v != 2 && v != 3 && v != static_cast<std::uintptr_t>(-1)) {
                    return p;
                }
            }
            if (opengl_mod != nullptr) {
                return reinterpret_cast<void*>(::GetProcAddress(opengl_mod, name));
            }
#endif
            return nullptr;
        };

        bool ok = true;
        const auto bind = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(resolve(name));
            ok = ok && (fn != nullptr);
        };
        bind(GetIntegerv, "glGetIntegerv");
        bind(IsEnabled, "glIsEnabled");
        bind(Enable, "glEnable");
        bind(Disable, "glDisable");
        bind(Viewport, "glViewport");
        bind(Scissor, "glScissor");
        bind(BlendFuncSeparate, "glBlendFuncSeparate");
        bind(BlendEquationSeparate, "glBlendEquationSeparate");
        bind(PolygonMode, "glPolygonMode");
        bind(PixelStorei, "glPixelStorei");
        bind(ActiveTexture, "glActiveTexture");
        bind(GenTextures, "glGenTextures");
        bind(DeleteTextures, "glDeleteTextures");
        bind(BindTexture, "glBindTexture");
        bind(TexImage2D, "glTexImage2D");
        bind(TexSubImage2D, "glTexSubImage2D");
        bind(TexParameteri, "glTexParameteri");
        bind(TexBuffer, "glTexBuffer");
        bind(GenSamplers, "glGenSamplers");
        bind(DeleteSamplers, "glDeleteSamplers");
        bind(BindSampler, "glBindSampler");
        bind(SamplerParameteri, "glSamplerParameteri");
        bind(GenBuffers, "glGenBuffers");
        bind(DeleteBuffers, "glDeleteBuffers");
        bind(BindBuffer, "glBindBuffer");
        bind(BufferData, "glBufferData");
        bind(BufferSubData, "glBufferSubData");
        bind(GenVertexArrays, "glGenVertexArrays");
        bind(DeleteVertexArrays, "glDeleteVertexArrays");
        bind(BindVertexArray, "glBindVertexArray");
        bind(EnableVertexAttribArray, "glEnableVertexAttribArray");
        bind(VertexAttribPointer, "glVertexAttribPointer");
        bind(VertexAttribIPointer, "glVertexAttribIPointer");
        bind(CreateShader, "glCreateShader");
        bind(DeleteShader, "glDeleteShader");
        bind(ShaderSource, "glShaderSource");
        bind(CompileShader, "glCompileShader");
        bind(GetShaderiv, "glGetShaderiv");
        bind(CreateProgram, "glCreateProgram");
        bind(DeleteProgram, "glDeleteProgram");
        bind(AttachShader, "glAttachShader");
        bind(LinkProgram, "glLinkProgram");
        bind(GetProgramiv, "glGetProgramiv");
        bind(UseProgram, "glUseProgram");
        bind(GetUniformLocation, "glGetUniformLocation");
        bind(Uniform1i, "glUniform1i");
        bind(Uniform4fv, "glUniform4fv");
        bind(DrawArrays, "glDrawArrays");
        bind(DrawElementsBaseVertex, "glDrawElementsBaseVertex");
        bind(GenFramebuffers, "glGenFramebuffers");
        bind(DeleteFramebuffers, "glDeleteFramebuffers");
        bind(BindFramebuffer, "glBindFramebuffer");
        bind(FramebufferTexture2D, "glFramebufferTexture2D");
        bind(CheckFramebufferStatus, "glCheckFramebufferStatus");
        bind(BlitFramebuffer, "glBlitFramebuffer");

        GetGraphicsResetStatus = reinterpret_cast<decltype(GetGraphicsResetStatus)>(resolve("glGetGraphicsResetStatus"));
        if (GetGraphicsResetStatus == nullptr) {
            GetGraphicsResetStatus = reinterpret_cast<decltype(GetGraphicsResetStatus)>(resolve("glGetGraphicsResetStatusARB"));
        }
        return ok;
    }
};

// OpenGL 3.3 Core shaders matching strata/shaders/ui.hlsl.
// Shapes are stored in a GL_RGBA32UI texture buffer (5 texels = 80 bytes per shape_record) so OpenGL 3.3 Core
// needs neither SSBOs (4.3) nor UBO size limits, and integer storage prevents IEEE NaN canonicalization on packed colours.
constexpr const char* kVsMain = R"glsl(
#version 330 core
layout(location = 0) in vec2  a_pos;
layout(location = 1) in uvec2 a_uv;
layout(location = 2) in vec4  a_col;

uniform vec4 u_transform; // xy: scale, zw: translate

out vec2      v_uv;
out vec4      v_col;
flat out uint v_shape_index;

void main()
{
    gl_Position = vec4(a_pos * u_transform.xy + u_transform.zw, 0.0, 1.0);
    v_col = a_col;
    if ((a_uv.y & 0x8000u) != 0u) {
        v_uv          = vec2(0.0);
        v_shape_index = a_uv.x | ((a_uv.y & 0x7fffu) << 16u);
    } else {
        v_uv          = vec2(a_uv) * (1.0 / 32767.0);
        v_shape_index = 0xffffffffu;
    }
}
)glsl";

constexpr const char* kFsCommon = R"glsl(
#version 330 core
in vec2      v_uv;
in vec4      v_col;
flat in uint v_shape_index;

uniform vec4 u_output; // x: output_space, y: white level, z: text contrast
uniform vec4 u_view;   // xy: origin, z: display_size.y (for flipping gl_FragCoord.y to top-down)

uniform sampler2D      u_atlas;
uniform usamplerBuffer u_shapes;
uniform sampler2D      u_image;
uniform sampler2D      u_blur_src;

uniform vec4 u_bp[5];

out vec4 o_color;

struct shape {
    vec2  center;
    vec2  half_size;
    vec4  radius;
    uint  fill_top;
    uint  fill_bottom;
    uint  border_color;
    uint  shadow_color;
    float border_width;
    float shadow_blur;
    vec2  shadow_offset;
    vec2  gradient_dir;
    float gradient_kind;
    float extra;
};

shape load_shape(uint idx)
{
    int   base = int(idx) * 5;
    uvec4 t0   = texelFetch(u_shapes, base + 0);
    uvec4 t1   = texelFetch(u_shapes, base + 1);
    uvec4 t2   = texelFetch(u_shapes, base + 2);
    uvec4 t3   = texelFetch(u_shapes, base + 3);
    uvec4 t4   = texelFetch(u_shapes, base + 4);
    shape s;
    s.center        = uintBitsToFloat(t0.xy);
    s.half_size     = uintBitsToFloat(t0.zw);
    s.radius        = uintBitsToFloat(t1);
    s.fill_top      = t2.x;
    s.fill_bottom   = t2.y;
    s.border_color  = t2.z;
    s.shadow_color  = t2.w;
    s.border_width  = uintBitsToFloat(t3.x);
    s.shadow_blur   = uintBitsToFloat(t3.y);
    s.shadow_offset = uintBitsToFloat(t3.zw);
    s.gradient_dir  = uintBitsToFloat(t4.xy);
    s.gradient_kind = uintBitsToFloat(t4.z);
    s.extra         = uintBitsToFloat(t4.w);
    return s;
}

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
    if (u_output.x < 0.5) { return c; }
    vec3 l = srgb_to_linear(clamp(c, 0.0, 1.0));
    if (u_output.x < 1.5) { return l; }
    if (u_output.x < 2.5) { return l * u_output.y; }
    vec3 bt2020 = vec3(
        dot(vec3(0.6274040, 0.3292820, 0.0433136), l),
        dot(vec3(0.0690970, 0.9195400, 0.0113612), l),
        dot(vec3(0.0163916, 0.0880132, 0.8955950), l)
    );
    return pq_encode(bt2020 * u_output.y);
}

vec4 encode_output(vec4 premul)
{
    if (u_output.x < 0.5 || premul.a <= 0.0) { return premul; }
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
)glsl";

constexpr const char* kFsMainBody = R"glsl(
void main()
{
    if (v_shape_index != 0xffffffffu) {
        vec2 pixel = vec2(gl_FragCoord.x, u_view.z - gl_FragCoord.y);
        o_color = encode_output(shade_shape(load_shape(v_shape_index), pixel + u_view.xy));
        return;
    }
    float coverage = textureLod(u_atlas, v_uv, 0.0).r;
    if (u_output.z > 0.0) {
        float k  = u_output.z * dot(v_col.rgb, vec3(0.2126, 0.7152, 0.0722));
        coverage = coverage * (1.0 + k) / (1.0 + k * coverage);
    }
    float a = v_col.a * coverage;
    o_color = encode_output(vec4(v_col.rgb * a, a));
}
)glsl";

constexpr const char* kFsImageBody = R"glsl(
void main()
{
    shape s     = load_shape(v_shape_index);
    vec2  pixel = vec2(gl_FragCoord.x, u_view.z - gl_FragCoord.y);
    vec2  p     = pixel + u_view.xy - s.center;
    float cov   = clamp(0.5 - sd_round_box(p, s.half_size, s.radius), 0.0, 1.0);

    vec2 t   = clamp(p / (2.0 * s.half_size) + 0.5, 0.0, 1.0);
    vec2 uv0 = s.shadow_offset;
    vec2 uv1 = vec2(s.border_width, s.shadow_blur);
    vec2 uv  = mix(uv0, uv1, t);

    vec2  dim = vec2(textureSize(u_image, 0));
    vec2  texels_per_pixel = abs(uv1 - uv0) * dim / max(2.0 * s.half_size, vec2(1.0));
    float lod = max(log2(max(max(texels_per_pixel.x, texels_per_pixel.y), 1.0e-4)), 0.0);
    vec4  c   = textureLod(u_image, uv, lod) * v_col;

    float a = c.a * cov;
    o_color = encode_output(vec4(c.rgb * a, a));
}
)glsl";

constexpr const char* kFsBackdropBody = R"glsl(
void main()
{
    shape s     = load_shape(v_shape_index);
    vec2  pixel = vec2(gl_FragCoord.x, u_view.z - gl_FragCoord.y);
    vec2  p     = pixel + u_view.xy - s.center;
    float cov   = clamp(0.5 - sd_round_box(p, s.half_size, s.radius), 0.0, 1.0);

    vec2  uv  = gl_FragCoord.xy / u_bp[0].xy * u_bp[0].zw;
    vec3  bg  = textureLod(u_blur_src, uv, 0.0).rgb;
    float lum = dot(bg, vec3(0.2126, 0.7152, 0.0722));
    bg = mix(vec3(lum), bg, s.shadow_offset.x) * s.shadow_offset.y;
    bg = u_output.x < 1.5 ? clamp(bg, 0.0, 1.0) : max(bg, vec3(0.0));
    vec4 tint = unpack_color(s.fill_top);
    vec3 rgb  = mix(bg, encode_rgb(tint.rgb), tint.a);

    float grain = fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    rgb = max(rgb + grain * s.extra * 0.5, vec3(0.0));
    o_color = vec4(rgb * cov, cov);
}
)glsl";

constexpr const char* kVsFullscreen = R"glsl(
#version 330 core
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

constexpr const char* kFsBlurDown = R"glsl(
#version 330 core
uniform sampler2D u_blur_src;
uniform vec4      u_bp[5];
out vec4          o_color;

void main()
{
    vec2 uv = gl_FragCoord.xy / u_bp[0].xy * u_bp[0].zw;
    vec2 o  = u_bp[1].xy * u_bp[2].x;
    vec4 c  = textureLod(u_blur_src, clamp(uv + vec2(-o.x, -o.y), vec2(0.0), u_bp[1].zw), 0.0)
            + textureLod(u_blur_src, clamp(uv + vec2( o.x, -o.y), vec2(0.0), u_bp[1].zw), 0.0)
            + textureLod(u_blur_src, clamp(uv + vec2(-o.x,  o.y), vec2(0.0), u_bp[1].zw), 0.0)
            + textureLod(u_blur_src, clamp(uv + vec2( o.x,  o.y), vec2(0.0), u_bp[1].zw), 0.0);
    o_color = c * 0.25;
}
)glsl";

constexpr const char* kFsBlurGauss = R"glsl(
#version 330 core
uniform sampler2D u_blur_src;
uniform vec4      u_bp[5];
out vec4          o_color;

void main()
{
    vec2 uv  = gl_FragCoord.xy / u_bp[0].xy * u_bp[0].zw;
    vec4 sum = textureLod(u_blur_src, uv, 0.0) * u_bp[2].z;
    vec2 w   = u_bp[3].xz;
    vec2 o   = u_bp[3].yw;
    sum += (textureLod(u_blur_src, clamp(uv + u_bp[1].xy * o.x, vec2(0.0), u_bp[1].zw), 0.0) +
            textureLod(u_blur_src, clamp(uv - u_bp[1].xy * o.x, vec2(0.0), u_bp[1].zw), 0.0)) * w.x;
    sum += (textureLod(u_blur_src, clamp(uv + u_bp[1].xy * o.y, vec2(0.0), u_bp[1].zw), 0.0) +
            textureLod(u_blur_src, clamp(uv - u_bp[1].xy * o.y, vec2(0.0), u_bp[1].zw), 0.0)) * w.y;
    sum += (textureLod(u_blur_src, clamp(uv + u_bp[1].xy * u_bp[4].y, vec2(0.0), u_bp[1].zw), 0.0) +
            textureLod(u_blur_src, clamp(uv - u_bp[1].xy * u_bp[4].y, vec2(0.0), u_bp[1].zw), 0.0)) * u_bp[4].x;
    o_color = sum;
}
)glsl";

[[nodiscard]] GLuint compile_shader(const gl_procs& gl, GLenum stage, const char* const* parts, GLsizei count) noexcept
{
    const GLuint s = gl.CreateShader(stage);
    if (s == 0) { return 0; }
    gl.ShaderSource(s, count, parts, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, kGL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

[[nodiscard]] GLuint link_program(const gl_procs& gl, GLuint vs, GLuint fs) noexcept
{
    if (vs == 0 || fs == 0) { return 0; }
    const GLuint p = gl.CreateProgram();
    if (p == 0) { return 0; }
    gl.AttachShader(p, vs);
    gl.AttachShader(p, fs);
    gl.LinkProgram(p);
    GLint ok = 0;
    gl.GetProgramiv(p, kGL_LINK_STATUS, &ok);
    if (ok == 0) {
        gl.DeleteProgram(p);
        return 0;
    }
    return p;
}

// saves and restores pixel-unpack state around texture uploads
class unpack_guard {
public:
    explicit unpack_guard(const gl_procs& gl) noexcept : gl_{gl}
    {
        gl_.GetIntegerv(kGL_PIXEL_UNPACK_BUFFER_BINDING, &pbo_);
        gl_.GetIntegerv(kGL_UNPACK_ALIGNMENT, &align_);
        gl_.GetIntegerv(kGL_UNPACK_ROW_LENGTH, &row_len_);
        gl_.GetIntegerv(kGL_UNPACK_SKIP_ROWS, &skip_rows_);
        gl_.GetIntegerv(kGL_UNPACK_SKIP_PIXELS, &skip_pixels_);
        if (pbo_ != 0) { gl_.BindBuffer(kGL_PIXEL_UNPACK_BUFFER, 0); }
        gl_.PixelStorei(kGL_UNPACK_ALIGNMENT, 1);
        gl_.PixelStorei(kGL_UNPACK_ROW_LENGTH, 0);
        gl_.PixelStorei(kGL_UNPACK_SKIP_ROWS, 0);
        gl_.PixelStorei(kGL_UNPACK_SKIP_PIXELS, 0);
    }
    ~unpack_guard()
    {
        gl_.PixelStorei(kGL_UNPACK_ALIGNMENT, align_);
        gl_.PixelStorei(kGL_UNPACK_ROW_LENGTH, row_len_);
        gl_.PixelStorei(kGL_UNPACK_SKIP_ROWS, skip_rows_);
        gl_.PixelStorei(kGL_UNPACK_SKIP_PIXELS, skip_pixels_);
        if (pbo_ != 0) { gl_.BindBuffer(kGL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(pbo_)); }
    }
    unpack_guard(const unpack_guard&)            = delete;
    unpack_guard& operator=(const unpack_guard&) = delete;

private:
    const gl_procs& gl_;
    GLint           pbo_{};
    GLint           align_{4};
    GLint           row_len_{};
    GLint           skip_rows_{};
    GLint           skip_pixels_{};
};

// snapshot of every GL state render() touches; restored on scope exit
class state_guard {
public:
    explicit state_guard(const gl_procs& gl) noexcept : gl_{gl}
    {
        gl_.GetIntegerv(kGL_CURRENT_PROGRAM, &program_);
        gl_.GetIntegerv(kGL_VERTEX_ARRAY_BINDING, &vao_);
        gl_.GetIntegerv(kGL_ARRAY_BUFFER_BINDING, &vbo_);
        gl_.GetIntegerv(kGL_ELEMENT_ARRAY_BUFFER_BINDING, &ebo_);
        gl_.GetIntegerv(kGL_ACTIVE_TEXTURE, &active_tex_);
        for (int i = 0; i < 4; ++i) {
            gl_.ActiveTexture(kGL_TEXTURE0 + static_cast<GLenum>(i));
            gl_.GetIntegerv(kGL_TEXTURE_BINDING_2D, &tex2d_[i]);
            gl_.GetIntegerv(kGL_SAMPLER_BINDING, &samplers_[i]);
        }
        gl_.ActiveTexture(kGL_TEXTURE0 + 1);
        gl_.GetIntegerv(kGL_TEXTURE_BINDING_BUFFER, &tex_buf_);
        gl_.GetIntegerv(kGL_VIEWPORT, viewport_);
        gl_.GetIntegerv(kGL_SCISSOR_BOX, scissor_);
        gl_.GetIntegerv(kGL_POLYGON_MODE, polygon_mode_);
        gl_.GetIntegerv(kGL_BLEND_SRC_RGB, &blend_src_rgb_);
        gl_.GetIntegerv(kGL_BLEND_DST_RGB, &blend_dst_rgb_);
        gl_.GetIntegerv(kGL_BLEND_SRC_ALPHA, &blend_src_a_);
        gl_.GetIntegerv(kGL_BLEND_DST_ALPHA, &blend_dst_a_);
        gl_.GetIntegerv(kGL_BLEND_EQUATION_RGB, &blend_eq_rgb_);
        gl_.GetIntegerv(kGL_BLEND_EQUATION_ALPHA, &blend_eq_a_);
        gl_.GetIntegerv(kGL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo_);
        gl_.GetIntegerv(kGL_READ_FRAMEBUFFER_BINDING, &read_fbo_);
        blend_   = gl_.IsEnabled(kGL_BLEND);
        cull_    = gl_.IsEnabled(kGL_CULL_FACE);
        depth_   = gl_.IsEnabled(kGL_DEPTH_TEST);
        stencil_ = gl_.IsEnabled(kGL_STENCIL_TEST);
        scissor_on_ = gl_.IsEnabled(kGL_SCISSOR_TEST);
    }

    ~state_guard()
    {
        gl_.UseProgram(static_cast<GLuint>(program_));
        gl_.BindVertexArray(static_cast<GLuint>(vao_));
        gl_.BindBuffer(kGL_ARRAY_BUFFER, static_cast<GLuint>(vbo_));
        gl_.BindBuffer(kGL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(ebo_));
        for (int i = 0; i < 4; ++i) {
            gl_.ActiveTexture(kGL_TEXTURE0 + static_cast<GLenum>(i));
            gl_.BindTexture(kGL_TEXTURE_2D, static_cast<GLuint>(tex2d_[i]));
            gl_.BindSampler(static_cast<GLuint>(i), static_cast<GLuint>(samplers_[i]));
        }
        gl_.ActiveTexture(kGL_TEXTURE0 + 1);
        gl_.BindTexture(kGL_TEXTURE_BUFFER, static_cast<GLuint>(tex_buf_));
        gl_.ActiveTexture(static_cast<GLenum>(active_tex_));
        gl_.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo_));
        gl_.BindFramebuffer(kGL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo_));
        gl_.Viewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        gl_.Scissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
        gl_.PolygonMode(kGL_FRONT_AND_BACK, static_cast<GLenum>(polygon_mode_[0]));
        gl_.BlendEquationSeparate(static_cast<GLenum>(blend_eq_rgb_), static_cast<GLenum>(blend_eq_a_));
        gl_.BlendFuncSeparate(static_cast<GLenum>(blend_src_rgb_), static_cast<GLenum>(blend_dst_rgb_),
                              static_cast<GLenum>(blend_src_a_), static_cast<GLenum>(blend_dst_a_));
        if (blend_) { gl_.Enable(kGL_BLEND); } else { gl_.Disable(kGL_BLEND); }
        if (cull_) { gl_.Enable(kGL_CULL_FACE); } else { gl_.Disable(kGL_CULL_FACE); }
        if (depth_) { gl_.Enable(kGL_DEPTH_TEST); } else { gl_.Disable(kGL_DEPTH_TEST); }
        if (stencil_) { gl_.Enable(kGL_STENCIL_TEST); } else { gl_.Disable(kGL_STENCIL_TEST); }
        if (scissor_on_) { gl_.Enable(kGL_SCISSOR_TEST); } else { gl_.Disable(kGL_SCISSOR_TEST); }
    }

    state_guard(const state_guard&)            = delete;
    state_guard& operator=(const state_guard&) = delete;

private:
    const gl_procs& gl_;
    GLint           program_{};
    GLint           vao_{};
    GLint           vbo_{};
    GLint           ebo_{};
    GLint           active_tex_{};
    GLint           tex2d_[4]{};
    GLint           samplers_[4]{};
    GLint           tex_buf_{};
    GLint           viewport_[4]{};
    GLint           scissor_[4]{};
    GLint           polygon_mode_[2]{};
    GLint           blend_src_rgb_{};
    GLint           blend_dst_rgb_{};
    GLint           blend_src_a_{};
    GLint           blend_dst_a_{};
    GLint           blend_eq_rgb_{};
    GLint           blend_eq_a_{};
    GLint           draw_fbo_{};
    GLint           read_fbo_{};
    GLboolean       blend_{};
    GLboolean       cull_{};
    GLboolean       depth_{};
    GLboolean       stencil_{};
    GLboolean       scissor_on_{};
};

struct gl_format_info {
    GLint  internal_fmt;
    GLenum format;
    GLenum type;
};

[[nodiscard]] gl_format_info gl_format_of(texture_layout l) noexcept
{
    switch (l) {
    case texture_layout::bgra8:   return {static_cast<GLint>(kGL_RGBA8), kGL_BGRA, kGL_UNSIGNED_BYTE};
    case texture_layout::rgba16f: return {static_cast<GLint>(kGL_RGBA16F), kGL_RGBA, kGL_HALF_FLOAT};
    default:                      return {static_cast<GLint>(kGL_RGBA8), kGL_RGBA, kGL_UNSIGNED_BYTE};
    }
}

} // namespace

struct gl_renderer::impl {
    gl_procs gl;

    struct ui_program {
        GLuint id{};
        GLint  u_transform{-1};
        GLint  u_output{-1};
        GLint  u_view{-1};
        GLint  u_bp{-1};
    };

    ui_program prog_main;
    ui_program prog_image;
    ui_program prog_backdrop;
    GLuint     prog_blur_down{};
    GLint      u_blur_down_bp{-1};
    GLuint     prog_blur_gauss{};
    GLint      u_blur_gauss_bp{-1};

    GLuint vao{};
    GLuint vbo{};
    GLuint ibo{};
    GLuint shape_bo{};
    GLuint shape_tex{};
    GLuint sampler{};
    GLuint atlas_tex{};
    u32    atlas_w{}, atlas_h{};

    std::size_t vb_capacity{};
    std::size_t ib_capacity{};
    std::size_t shape_capacity{};
    u64         uploaded_hash{};

    struct texture_slot {
        GLuint        tex{}; // 0 = free slot
        bool          owned{};
        bool          updatable{};
        texture_image image;
    };
    std::vector<texture_slot> textures;

    // backdrop blur targets
    GLuint snap_tex{};
    GLuint snap_fbo{};
    u32    snap_w{}, snap_h{};
    GLint  snap_format{};
    GLuint blur_tex[2]{};
    GLuint blur_fbo[2]{};
    u32    blur_w{}, blur_h{};
    GLint  blur_format{};
    bool   blur_supported{};

    output_desc output{};
    bool        restore_state{true};

    struct blur_constants {
        float bp[20]{};
    };

    void release() noexcept
    {
        for (texture_slot& t : textures) {
            t.image.wipe();
            if (t.owned && t.tex != 0) { gl.DeleteTextures(1, &t.tex); }
        }
        textures.clear();
        if (atlas_tex != 0) { gl.DeleteTextures(1, &atlas_tex); atlas_tex = 0; }
        if (shape_tex != 0) { gl.DeleteTextures(1, &shape_tex); shape_tex = 0; }
        if (snap_tex != 0) { gl.DeleteTextures(1, &snap_tex); snap_tex = 0; }
        if (snap_fbo != 0) { gl.DeleteFramebuffers(1, &snap_fbo); snap_fbo = 0; }
        for (int k = 0; k < 2; ++k) {
            if (blur_tex[k] != 0) { gl.DeleteTextures(1, &blur_tex[k]); blur_tex[k] = 0; }
            if (blur_fbo[k] != 0) { gl.DeleteFramebuffers(1, &blur_fbo[k]); blur_fbo[k] = 0; }
        }
        if (sampler != 0) { gl.DeleteSamplers(1, &sampler); sampler = 0; }
        if (shape_bo != 0) { gl.DeleteBuffers(1, &shape_bo); shape_bo = 0; }
        if (ibo != 0) { gl.DeleteBuffers(1, &ibo); ibo = 0; }
        if (vbo != 0) { gl.DeleteBuffers(1, &vbo); vbo = 0; }
        if (vao != 0) { gl.DeleteVertexArrays(1, &vao); vao = 0; }
        if (prog_main.id != 0) { gl.DeleteProgram(prog_main.id); prog_main.id = 0; }
        if (prog_image.id != 0) { gl.DeleteProgram(prog_image.id); prog_image.id = 0; }
        if (prog_backdrop.id != 0) { gl.DeleteProgram(prog_backdrop.id); prog_backdrop.id = 0; }
        if (prog_blur_down != 0) { gl.DeleteProgram(prog_blur_down); prog_blur_down = 0; }
        if (prog_blur_gauss != 0) { gl.DeleteProgram(prog_blur_gauss); prog_blur_gauss = 0; }
    }

    texture_id add_texture(texture_slot&& slot)
    {
        for (std::size_t i = 0; i < textures.size(); ++i) {
            if (textures[i].tex == 0) {
                textures[i] = std::move(slot);
                return static_cast<texture_id>(i + 1);
            }
        }
        textures.push_back(std::move(slot));
        return static_cast<texture_id>(textures.size());
    }

    [[nodiscard]] bool init_ui_program(ui_program& out, GLuint vs, const char* fs_body) noexcept
    {
        const char* parts[2] = {kFsCommon, fs_body};
        const GLuint fs = compile_shader(gl, kGL_FRAGMENT_SHADER, parts, 2);
        const GLuint prog = link_program(gl, vs, fs);
        if (fs != 0) { gl.DeleteShader(fs); }
        if (prog == 0) { return false; }
        out.id          = prog;
        out.u_transform = gl.GetUniformLocation(prog, "u_transform");
        out.u_output    = gl.GetUniformLocation(prog, "u_output");
        out.u_view      = gl.GetUniformLocation(prog, "u_view");
        out.u_bp        = gl.GetUniformLocation(prog, "u_bp");
        gl.UseProgram(prog);
        if (const GLint loc = gl.GetUniformLocation(prog, "u_atlas"); loc >= 0) { gl.Uniform1i(loc, 0); }
        if (const GLint loc = gl.GetUniformLocation(prog, "u_shapes"); loc >= 0) { gl.Uniform1i(loc, 1); }
        if (const GLint loc = gl.GetUniformLocation(prog, "u_image"); loc >= 0) { gl.Uniform1i(loc, 2); }
        if (const GLint loc = gl.GetUniformLocation(prog, "u_blur_src"); loc >= 0) { gl.Uniform1i(loc, 3); }
        return true;
    }

    [[nodiscard]] bool ensure_blur_targets(u32 w, u32 h) noexcept
    {
        const GLint fmt = (output.space == output_space::scrgb || output.space == output_space::hdr10)
                              ? static_cast<GLint>(kGL_RGBA16F)
                              : static_cast<GLint>(kGL_RGBA8);
        const GLenum type = fmt == static_cast<GLint>(kGL_RGBA16F) ? kGL_HALF_FLOAT : kGL_UNSIGNED_BYTE;

        if (snap_tex == 0 || snap_w != w || snap_h != h || snap_format != fmt) {
            if (snap_tex == 0) { gl.GenTextures(1, &snap_tex); }
            if (snap_fbo == 0) { gl.GenFramebuffers(1, &snap_fbo); }
            gl.ActiveTexture(kGL_TEXTURE0 + 3);
            gl.BindTexture(kGL_TEXTURE_2D, snap_tex);
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_BASE_LEVEL, 0);
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAX_LEVEL, 0);
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MIN_FILTER, static_cast<GLint>(kGL_LINEAR));
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAG_FILTER, static_cast<GLint>(kGL_LINEAR));
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_S, static_cast<GLint>(kGL_CLAMP_TO_EDGE));
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_T, static_cast<GLint>(kGL_CLAMP_TO_EDGE));
            gl.TexImage2D(kGL_TEXTURE_2D, 0, fmt, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, kGL_RGBA, type, nullptr);
            gl.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, snap_fbo);
            gl.FramebufferTexture2D(kGL_DRAW_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, kGL_TEXTURE_2D, snap_tex, 0);
            if (gl.CheckFramebufferStatus(kGL_DRAW_FRAMEBUFFER) != kGL_FRAMEBUFFER_COMPLETE) {
                return false;
            }
            snap_w = w; snap_h = h; snap_format = fmt;
        }

        const u32 lw = (w + 1) / 2;
        const u32 lh = (h + 1) / 2;
        if (blur_tex[0] == 0 || blur_w != lw || blur_h != lh || blur_format != fmt) {
            if (blur_tex[0] == 0) {
                gl.GenTextures(2, blur_tex);
                gl.GenFramebuffers(2, blur_fbo);
            }
            gl.ActiveTexture(kGL_TEXTURE0 + 3);
            for (int k = 0; k < 2; ++k) {
                gl.BindTexture(kGL_TEXTURE_2D, blur_tex[k]);
                gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_BASE_LEVEL, 0);
                gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAX_LEVEL, 0);
                gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MIN_FILTER, static_cast<GLint>(kGL_LINEAR));
                gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAG_FILTER, static_cast<GLint>(kGL_LINEAR));
                gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_S, static_cast<GLint>(kGL_CLAMP_TO_EDGE));
                gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_T, static_cast<GLint>(kGL_CLAMP_TO_EDGE));
                gl.TexImage2D(kGL_TEXTURE_2D, 0, fmt, static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0, kGL_RGBA, type, nullptr);
                gl.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, blur_fbo[k]);
                gl.FramebufferTexture2D(kGL_DRAW_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, kGL_TEXTURE_2D, blur_tex[k], 0);
                if (gl.CheckFramebufferStatus(kGL_DRAW_FRAMEBUFFER) != kGL_FRAMEBUFFER_COMPLETE) {
                    return false;
                }
            }
            blur_w = lw; blur_h = lh; blur_format = fmt;
        }
        return true;
    }

    void blur_pass(GLuint dst_fbo, u32 vw, u32 vh, GLuint src_tex, const blur_constants& c, GLuint prog, GLint u_bp,
                   const internal::blur_plan& plan) noexcept
    {
        gl.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, dst_fbo);
        gl.Viewport(0, 0, static_cast<GLsizei>(vw), static_cast<GLsizei>(vh));
        // convert top-down plan region to bottom-up OpenGL scissor
        gl.Scissor(static_cast<GLint>(plan.lx0), static_cast<GLint>(vh - plan.ly1),
                   static_cast<GLsizei>(plan.lx1 - plan.lx0), static_cast<GLsizei>(plan.ly1 - plan.ly0));
        gl.UseProgram(prog);
        gl.Uniform4fv(u_bp, 5, c.bp);
        gl.ActiveTexture(kGL_TEXTURE0 + 3);
        gl.BindTexture(kGL_TEXTURE_2D, src_tex);
        gl.DrawArrays(kGL_TRIANGLES, 0, 3);
    }

    [[nodiscard]] bool run_blur(GLuint host_fbo, u32 target_w, u32 target_h, float radius_px, const rect& panel,
                                u32& out_vw, u32& out_vh, rect& out_exact) noexcept
    {
        if (!ensure_blur_targets(target_w, target_h)) {
            gl.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, host_fbo);
            return false;
        }
        const internal::blur_plan plan = internal::plan_blur(radius_px, panel, snap_w, snap_h);
        if (plan.x1 <= plan.x0 || plan.y1 <= plan.y0 || plan.lx1 <= plan.lx0 || plan.ly1 <= plan.ly0) {
            gl.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, host_fbo);
            return false;
        }

        // copy the target's region into snap_tex (bottom-up coordinates; disable scissor so the blit is unclipped)
        gl.Disable(kGL_SCISSOR_TEST);
        gl.BindFramebuffer(kGL_READ_FRAMEBUFFER, host_fbo);
        gl.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, snap_fbo);
        const GLint bx0 = static_cast<GLint>(plan.x0);
        const GLint bx1 = static_cast<GLint>(plan.x1);
        const GLint by0 = static_cast<GLint>(snap_h - plan.y1);
        const GLint by1 = static_cast<GLint>(snap_h - plan.y0);
        gl.BlitFramebuffer(bx0, by0, bx1, by1, bx0, by0, bx1, by1, kGL_COLOR_BUFFER_BIT, kGL_NEAREST);
        gl.Enable(kGL_SCISSOR_TEST);
        gl.Disable(kGL_BLEND);

        const u32   ds = plan.ds;
        const u32   vw = std::max(1u, (snap_w + ds - 1) / ds);
        const u32   vh = std::max(1u, (snap_h + ds - 1) / ds);
        const float tw = static_cast<float>(blur_w);
        const float th = static_cast<float>(blur_h);

        blur_constants c;
        c.bp[0] = static_cast<float>(vw); c.bp[1] = static_cast<float>(vh); c.bp[2] = 1.0f; c.bp[3] = 1.0f;
        c.bp[4] = 1.0f / static_cast<float>(snap_w); c.bp[5] = 1.0f / static_cast<float>(snap_h);
        c.bp[6] = 1.0f - 0.5f * c.bp[4]; c.bp[7] = 1.0f - 0.5f * c.bp[5];
        c.bp[8] = static_cast<float>(ds) * 0.25f;
        blur_pass(blur_fbo[0], vw, vh, snap_tex, c, prog_blur_down, u_blur_down_bp, plan);

        c.bp[0] = static_cast<float>(vw); c.bp[1] = static_cast<float>(vh);
        c.bp[2] = static_cast<float>(vw) / tw; c.bp[3] = static_cast<float>(vh) / th;
        c.bp[6] = (static_cast<float>(vw) - 0.5f) / tw; c.bp[7] = (static_cast<float>(vh) - 0.5f) / th;
        c.bp[10] = plan.w0;
        c.bp[12] = plan.w[0]; c.bp[13] = plan.o[0]; c.bp[14] = plan.w[1]; c.bp[15] = plan.o[1];
        c.bp[16] = plan.w[2]; c.bp[17] = plan.o[2];
        for (int it = 0; it < plan.iterations; ++it) {
            c.bp[4] = 1.0f / tw; c.bp[5] = 0.0f;
            blur_pass(blur_fbo[1], vw, vh, blur_tex[0], c, prog_blur_gauss, u_blur_gauss_bp, plan);
            c.bp[4] = 0.0f; c.bp[5] = 1.0f / th;
            blur_pass(blur_fbo[0], vw, vh, blur_tex[1], c, prog_blur_gauss, u_blur_gauss_bp, plan);
        }

        // restore the host's framebuffer and UI render state
        gl.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, host_fbo);
        gl.BindFramebuffer(kGL_READ_FRAMEBUFFER, host_fbo);
        gl.Viewport(0, 0, static_cast<GLsizei>(target_w), static_cast<GLsizei>(target_h));
        gl.Enable(kGL_BLEND);
        out_vw    = vw;
        out_vh    = vh;
        out_exact = plan.exact;
        return true;
    }

    void bind_ui_uniforms(const ui_program& prog, const internal::ui_constants& uc, float display_h) noexcept
    {
        gl.UseProgram(prog.id);
        gl.Uniform4fv(prog.u_transform, 1, uc.transform);
        gl.Uniform4fv(prog.u_output, 1, uc.output);
        const float view[4] = {uc.view[0], uc.view[1], display_h, 0.0f};
        gl.Uniform4fv(prog.u_view, 1, view);
    }
};

gl_renderer::gl_renderer() noexcept = default;
gl_renderer::~gl_renderer() { destroy(); }
gl_renderer::gl_renderer(gl_renderer&&) noexcept            = default;
gl_renderer& gl_renderer::operator=(gl_renderer&&) noexcept = default;

bool gl_renderer::create(const font_atlas& atlas, proc_loader loader)
{
    destroy();
    auto p = std::make_unique<impl>();
    if (!p->gl.load(loader)) {
        return false;
    }
    const gl_procs& gl = p->gl;

    // preserve the caller's GL state across setup
    const state_guard guard(gl);

    const GLuint vs = compile_shader(gl, kGL_VERTEX_SHADER, &kVsMain, 1);
    const bool ui_ok = vs != 0 &&
                       p->init_ui_program(p->prog_main, vs, kFsMainBody) &&
                       p->init_ui_program(p->prog_image, vs, kFsImageBody);
    if (!ui_ok) {
        if (vs != 0) { gl.DeleteShader(vs); }
        p->release();
        return false;
    }

    // optional backdrop blur pipeline
    const GLuint vs_fs   = compile_shader(gl, kGL_VERTEX_SHADER, &kVsFullscreen, 1);
    const GLuint fs_down = compile_shader(gl, kGL_FRAGMENT_SHADER, &kFsBlurDown, 1);
    const GLuint fs_gaus = compile_shader(gl, kGL_FRAGMENT_SHADER, &kFsBlurGauss, 1);
    p->blur_supported = p->init_ui_program(p->prog_backdrop, vs, kFsBackdropBody) &&
                        (p->prog_blur_down = link_program(gl, vs_fs, fs_down)) != 0 &&
                        (p->prog_blur_gauss = link_program(gl, vs_fs, fs_gaus)) != 0;
    if (vs != 0) { gl.DeleteShader(vs); }
    if (vs_fs != 0) { gl.DeleteShader(vs_fs); }
    if (fs_down != 0) { gl.DeleteShader(fs_down); }
    if (fs_gaus != 0) { gl.DeleteShader(fs_gaus); }
    if (p->blur_supported) {
        gl.UseProgram(p->prog_blur_down);
        if (const GLint loc = gl.GetUniformLocation(p->prog_blur_down, "u_blur_src"); loc >= 0) { gl.Uniform1i(loc, 3); }
        p->u_blur_down_bp = gl.GetUniformLocation(p->prog_blur_down, "u_bp");
        gl.UseProgram(p->prog_blur_gauss);
        if (const GLint loc = gl.GetUniformLocation(p->prog_blur_gauss, "u_blur_src"); loc >= 0) { gl.Uniform1i(loc, 3); }
        p->u_blur_gauss_bp = gl.GetUniformLocation(p->prog_blur_gauss, "u_bp");
    }

    gl.GenVertexArrays(1, &p->vao);
    gl.GenBuffers(1, &p->vbo);
    gl.GenBuffers(1, &p->ibo);
    gl.GenBuffers(1, &p->shape_bo);
    gl.GenTextures(1, &p->shape_tex);
    gl.GenSamplers(1, &p->sampler);

    gl.BindVertexArray(p->vao);
    gl.BindBuffer(kGL_ARRAY_BUFFER, p->vbo);
    gl.BindBuffer(kGL_ELEMENT_ARRAY_BUFFER, p->ibo);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, kGL_FLOAT, kGL_FALSE, sizeof(vertex), reinterpret_cast<const void*>(offsetof(vertex, pos)));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribIPointer(1, 2, kGL_UNSIGNED_SHORT, sizeof(vertex), reinterpret_cast<const void*>(offsetof(vertex, u)));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 4, kGL_UNSIGNED_BYTE, kGL_TRUE, sizeof(vertex), reinterpret_cast<const void*>(offsetof(vertex, col)));

    gl.SamplerParameteri(p->sampler, kGL_TEXTURE_MIN_FILTER, static_cast<GLint>(kGL_LINEAR_MIPMAP_LINEAR));
    gl.SamplerParameteri(p->sampler, kGL_TEXTURE_MAG_FILTER, static_cast<GLint>(kGL_LINEAR));
    gl.SamplerParameteri(p->sampler, kGL_TEXTURE_WRAP_S, static_cast<GLint>(kGL_CLAMP_TO_EDGE));
    gl.SamplerParameteri(p->sampler, kGL_TEXTURE_WRAP_T, static_cast<GLint>(kGL_CLAMP_TO_EDGE));

    // initial dummy shape buffer so the texture buffer is always valid
    const shape_record dummy{};
    gl.BindBuffer(kGL_TEXTURE_BUFFER, p->shape_bo);
    gl.BufferData(kGL_TEXTURE_BUFFER, sizeof(dummy), &dummy, kGL_DYNAMIC_DRAW);
    p->shape_capacity = 1;
    gl.ActiveTexture(kGL_TEXTURE0 + 1);
    gl.BindTexture(kGL_TEXTURE_BUFFER, p->shape_tex);
    gl.TexBuffer(kGL_TEXTURE_BUFFER, kGL_RGBA32UI, p->shape_bo);

    // font atlas texture (r8)
    gl.GenTextures(1, &p->atlas_tex);
    gl.ActiveTexture(kGL_TEXTURE0);
    gl.BindTexture(kGL_TEXTURE_2D, p->atlas_tex);
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAX_LEVEL, 0);
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MIN_FILTER, static_cast<GLint>(kGL_LINEAR));
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAG_FILTER, static_cast<GLint>(kGL_LINEAR));
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_S, static_cast<GLint>(kGL_CLAMP_TO_EDGE));
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_T, static_cast<GLint>(kGL_CLAMP_TO_EDGE));
    {
        const unpack_guard ug(gl);
        gl.TexImage2D(kGL_TEXTURE_2D, 0, static_cast<GLint>(kGL_R8), static_cast<GLsizei>(atlas.width()),
                      static_cast<GLsizei>(atlas.height()), 0, kGL_RED, kGL_UNSIGNED_BYTE, atlas.pixels().data());
    }
    p->atlas_w = atlas.width();
    p->atlas_h = atlas.height();

    impl_ = std::move(p);
    return true;
}

void gl_renderer::destroy() noexcept
{
    if (impl_ != nullptr) {
        impl_->release();
        impl_.reset();
    }
}

bool gl_renderer::update_atlas(const font_atlas& atlas)
{
    if (impl_ == nullptr || atlas.pixels().empty()) { return false; }
    impl& s = *impl_;
    GLint prev_active{}, prev_tex{};
    s.gl.GetIntegerv(kGL_ACTIVE_TEXTURE, &prev_active);
    s.gl.ActiveTexture(kGL_TEXTURE0);
    s.gl.GetIntegerv(kGL_TEXTURE_BINDING_2D, &prev_tex);
    s.gl.BindTexture(kGL_TEXTURE_2D, s.atlas_tex);
    {
        const unpack_guard ug(s.gl);
        s.gl.TexImage2D(kGL_TEXTURE_2D, 0, static_cast<GLint>(kGL_R8), static_cast<GLsizei>(atlas.width()),
                        static_cast<GLsizei>(atlas.height()), 0, kGL_RED, kGL_UNSIGNED_BYTE, atlas.pixels().data());
    }
    s.atlas_w = atlas.width();
    s.atlas_h = atlas.height();
    s.gl.BindTexture(kGL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    s.gl.ActiveTexture(static_cast<GLenum>(prev_active));
    return true;
}

bool gl_renderer::update_atlas_region(const font_atlas& atlas, atlas_region r)
{
    if (impl_ == nullptr || r.empty() || atlas.pixels().empty()) { return false; }
    impl& s = *impl_;
    if (atlas.width() != s.atlas_w || atlas.height() != s.atlas_h || r.x + r.w > s.atlas_w || r.y + r.h > s.atlas_h) {
        return false;
    }
    GLint prev_active{}, prev_tex{};
    s.gl.GetIntegerv(kGL_ACTIVE_TEXTURE, &prev_active);
    s.gl.ActiveTexture(kGL_TEXTURE0);
    s.gl.GetIntegerv(kGL_TEXTURE_BINDING_2D, &prev_tex);
    s.gl.BindTexture(kGL_TEXTURE_2D, s.atlas_tex);
    {
        const unpack_guard ug(s.gl);
        s.gl.PixelStorei(kGL_UNPACK_ROW_LENGTH, static_cast<GLint>(s.atlas_w));
        const u8* first = atlas.pixels().data() + static_cast<std::size_t>(r.y) * s.atlas_w + r.x;
        s.gl.TexSubImage2D(kGL_TEXTURE_2D, 0, static_cast<GLint>(r.x), static_cast<GLint>(r.y),
                           static_cast<GLsizei>(r.w), static_cast<GLsizei>(r.h), kGL_RED, kGL_UNSIGNED_BYTE, first);
    }
    s.gl.BindTexture(kGL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    s.gl.ActiveTexture(static_cast<GLenum>(prev_active));
    return true;
}

texture_id gl_renderer::create_texture(u32 width, u32 height, std::span<const u8> rgba)
{
    return create_texture(texture_desc{width, height, texture_format::rgba8, 1, false}, rgba);
}

texture_id gl_renderer::create_texture(const texture_desc& desc, std::span<const u8> pixels)
{
    if (impl_ == nullptr) { return 0; }
    texture_image image;
    if (!image.create(desc, pixels)) { return 0; }

    impl& s = *impl_;
    const gl_format_info fi = gl_format_of(image.layout());
    const u32 levels = image.level_count();

    GLint prev_active{}, prev_tex{};
    s.gl.GetIntegerv(kGL_ACTIVE_TEXTURE, &prev_active);
    s.gl.ActiveTexture(kGL_TEXTURE0 + 2);
    s.gl.GetIntegerv(kGL_TEXTURE_BINDING_2D, &prev_tex);

    GLuint tex = 0;
    s.gl.GenTextures(1, &tex);
    if (tex == 0) {
        s.gl.ActiveTexture(static_cast<GLenum>(prev_active));
        image.wipe();
        return 0;
    }
    s.gl.BindTexture(kGL_TEXTURE_2D, tex);
    s.gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_BASE_LEVEL, 0);
    s.gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels - 1));
    s.gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MIN_FILTER, static_cast<GLint>(levels > 1 ? kGL_LINEAR_MIPMAP_LINEAR : kGL_LINEAR));
    s.gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAG_FILTER, static_cast<GLint>(kGL_LINEAR));
    s.gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_S, static_cast<GLint>(kGL_CLAMP_TO_EDGE));
    s.gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_T, static_cast<GLint>(kGL_CLAMP_TO_EDGE));
    {
        const unpack_guard ug(s.gl);
        for (u32 k = 0; k < levels; ++k) {
            s.gl.TexImage2D(kGL_TEXTURE_2D, static_cast<GLint>(k), fi.internal_fmt, static_cast<GLsizei>(image.width(k)),
                            static_cast<GLsizei>(image.height(k)), 0, fi.format, fi.type, image.pixels(k).data());
        }
    }

    s.gl.BindTexture(kGL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    s.gl.ActiveTexture(static_cast<GLenum>(prev_active));

    impl::texture_slot slot;
    slot.tex       = tex;
    slot.owned     = true;
    slot.updatable = desc.updatable;
    if (desc.updatable) { slot.image = std::move(image); }
    else                { image.wipe(); }
    return s.add_texture(std::move(slot));
}

bool gl_renderer::update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels)
{
    if (impl_ == nullptr || id == 0 || id > impl_->textures.size()) { return false; }
    impl& s = *impl_;
    impl::texture_slot& slot = s.textures[id - 1];
    if (slot.tex == 0 || !slot.updatable) { return false; }
    std::vector<texture_image::region> dirty;
    if (!slot.image.update(x, y, width, height, pixels, dirty)) { return false; }

    const gl_format_info fi  = gl_format_of(slot.image.layout());
    const u32            bpp = texture_layout_bytes(slot.image.layout());

    GLint prev_active{}, prev_tex{};
    s.gl.GetIntegerv(kGL_ACTIVE_TEXTURE, &prev_active);
    s.gl.ActiveTexture(kGL_TEXTURE0 + 2);
    s.gl.GetIntegerv(kGL_TEXTURE_BINDING_2D, &prev_tex);
    s.gl.BindTexture(kGL_TEXTURE_2D, slot.tex);
    {
        const unpack_guard ug(s.gl);
        for (const texture_image::region& r : dirty) {
            s.gl.PixelStorei(kGL_UNPACK_ROW_LENGTH, static_cast<GLint>(slot.image.width(r.level)));
            const u8* first = slot.image.pixels(r.level).data() +
                              static_cast<std::size_t>(r.y) * slot.image.pitch(r.level) +
                              static_cast<std::size_t>(r.x) * bpp;
            s.gl.TexSubImage2D(kGL_TEXTURE_2D, static_cast<GLint>(r.level), static_cast<GLint>(r.x), static_cast<GLint>(r.y),
                               static_cast<GLsizei>(r.w), static_cast<GLsizei>(r.h), fi.format, fi.type, first);
        }
    }
    s.gl.BindTexture(kGL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    s.gl.ActiveTexture(static_cast<GLenum>(prev_active));
    return true;
}

texture_id gl_renderer::register_texture(u32 gl_texture)
{
    if (impl_ == nullptr || gl_texture == 0) { return 0; }
    impl::texture_slot slot;
    slot.tex   = gl_texture;
    slot.owned = false;
    return impl_->add_texture(std::move(slot));
}

void gl_renderer::destroy_texture(texture_id id) noexcept
{
    if (impl_ != nullptr && id != 0 && id <= impl_->textures.size()) {
        impl::texture_slot& slot = impl_->textures[id - 1];
        slot.image.wipe();
        if (slot.owned && slot.tex != 0) { impl_->gl.DeleteTextures(1, &slot.tex); }
        slot = {};
    }
}

void gl_renderer::set_state_restore(bool on) noexcept
{
    if (impl_ != nullptr) { impl_->restore_state = on; }
}

bool gl_renderer::state_restore() const noexcept
{
    return impl_ != nullptr && impl_->restore_state;
}

void gl_renderer::set_output(const output_desc& output) noexcept
{
    if (impl_ != nullptr) { impl_->output = output; }
}

output_desc gl_renderer::output() const noexcept
{
    return impl_ != nullptr ? impl_->output : output_desc{};
}

bool gl_renderer::device_lost() const noexcept
{
    return impl_ != nullptr && impl_->gl.GetGraphicsResetStatus != nullptr &&
           impl_->gl.GetGraphicsResetStatus() != kGL_NO_ERROR;
}

void gl_renderer::render(const draw_data& data)
{
    if (impl_ == nullptr || data.commands.empty() || data.display_size.x <= 0.0f || data.display_size.y <= 0.0f) {
        return;
    }
    impl& s = *impl_;
    const gl_procs& gl = s.gl;

    std::optional<state_guard> guard;
    if (s.restore_state) { guard.emplace(gl); }

    GLint host_fbo_i = 0;
    gl.GetIntegerv(kGL_DRAW_FRAMEBUFFER_BINDING, &host_fbo_i);
    const auto host_fbo = static_cast<GLuint>(host_fbo_i);

    gl.BindVertexArray(s.vao);

    // upload geometry only when the content hash changed
    const bool have_geometry = data.content_hash != 0 && data.content_hash == s.uploaded_hash;
    if (!have_geometry) {
        const std::size_t vb_bytes = data.vertices.size_bytes();
        const std::size_t ib_bytes = data.indices.size_bytes();
        const std::size_t scount   = std::max<std::size_t>(data.shapes.size(), 1);

        gl.BindBuffer(kGL_ARRAY_BUFFER, s.vbo);
        if (vb_bytes > s.vb_capacity) {
            s.vb_capacity = vb_bytes + vb_bytes / 2 + 4096 * sizeof(vertex);
            gl.BufferData(kGL_ARRAY_BUFFER, static_cast<GLsizeiptr>(s.vb_capacity), nullptr, kGL_STREAM_DRAW);
        } else {
            // orphan buffer to avoid stalling on in-flight GPU draws
            gl.BufferData(kGL_ARRAY_BUFFER, static_cast<GLsizeiptr>(s.vb_capacity), nullptr, kGL_STREAM_DRAW);
        }
        if (vb_bytes != 0) { gl.BufferSubData(kGL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vb_bytes), data.vertices.data()); }

        gl.BindBuffer(kGL_ELEMENT_ARRAY_BUFFER, s.ibo);
        if (ib_bytes > s.ib_capacity) {
            s.ib_capacity = ib_bytes + ib_bytes / 2 + 8192 * sizeof(index_t);
            gl.BufferData(kGL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(s.ib_capacity), nullptr, kGL_STREAM_DRAW);
        } else {
            gl.BufferData(kGL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(s.ib_capacity), nullptr, kGL_STREAM_DRAW);
        }
        if (ib_bytes != 0) { gl.BufferSubData(kGL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(ib_bytes), data.indices.data()); }

        gl.BindBuffer(kGL_TEXTURE_BUFFER, s.shape_bo);
        if (scount > s.shape_capacity) {
            s.shape_capacity = scount + scount / 2 + 256;
            gl.BufferData(kGL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(s.shape_capacity * sizeof(shape_record)), nullptr, kGL_STREAM_DRAW);
            gl.ActiveTexture(kGL_TEXTURE0 + 1);
            gl.BindTexture(kGL_TEXTURE_BUFFER, s.shape_tex);
            gl.TexBuffer(kGL_TEXTURE_BUFFER, kGL_RGBA32UI, s.shape_bo);
        } else {
            gl.BufferData(kGL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(s.shape_capacity * sizeof(shape_record)), nullptr, kGL_STREAM_DRAW);
        }
        if (!data.shapes.empty()) {
            gl.BufferSubData(kGL_TEXTURE_BUFFER, 0, static_cast<GLsizeiptr>(data.shapes.size_bytes()), data.shapes.data());
        }
        s.uploaded_hash = data.content_hash;
    }

    const u32 target_w = static_cast<u32>(std::lround(data.display_size.x));
    const u32 target_h = static_cast<u32>(std::lround(data.display_size.y));
    gl.Viewport(0, 0, static_cast<GLsizei>(target_w), static_cast<GLsizei>(target_h));
    gl.Enable(kGL_BLEND);
    gl.BlendEquationSeparate(kGL_FUNC_ADD, kGL_FUNC_ADD);
    gl.BlendFuncSeparate(kGL_ONE, kGL_ONE_MINUS_SRC_ALPHA, kGL_ONE, kGL_ONE_MINUS_SRC_ALPHA);
    gl.Disable(kGL_CULL_FACE);
    gl.Disable(kGL_DEPTH_TEST);
    gl.Disable(kGL_STENCIL_TEST);
    gl.Enable(kGL_SCISSOR_TEST);
    gl.PolygonMode(kGL_FRONT_AND_BACK, kGL_FILL);

    gl.ActiveTexture(kGL_TEXTURE0);
    gl.BindTexture(kGL_TEXTURE_2D, s.atlas_tex);
    gl.BindSampler(0, s.sampler);
    gl.ActiveTexture(kGL_TEXTURE0 + 1);
    gl.BindTexture(kGL_TEXTURE_BUFFER, s.shape_tex);
    gl.BindSampler(1, 0);
    gl.BindSampler(2, s.sampler);
    gl.BindSampler(3, s.sampler);

    const internal::ui_constants uc = internal::make_ui_constants(data, s.output);
    s.bind_ui_uniforms(s.prog_main, uc, data.display_size.y);

    enum class ps_mode { main, image, backdrop };
    ps_mode    mode        = ps_mode::main;
    texture_id bound       = 0;
    bool       blur_dirty  = true;
    float      blur_radius = -1.0f;
    rect       blur_exact{};
    u32        blur_vw = 0, blur_vh = 0;
    bool       has_scissor = false;
    GLint      bound_sc[4]{};

    for (const draw_cmd& cmd : data.commands) {
        if (cmd.idx_count == 0 || cmd.clip.empty()) { continue; }
        const auto fit = [](f32 v, f32 hi) { return static_cast<GLint>(std::clamp(v, 0.0f, hi)); };
        const GLint left   = fit(cmd.clip.min.x - data.origin.x, data.display_size.x);
        const GLint top    = fit(cmd.clip.min.y - data.origin.y, data.display_size.y);
        const GLint right  = fit(cmd.clip.max.x - data.origin.x, data.display_size.x);
        const GLint bottom = fit(cmd.clip.max.y - data.origin.y, data.display_size.y);
        if (right <= left || bottom <= top) { continue; }

        ps_mode want = cmd.texture != 0 ? ps_mode::image : ps_mode::main;
        if (cmd.blur > 0.0f && s.blur_supported && data.origin.x == 0.0f && data.origin.y == 0.0f) {
            const rect panel = internal::backdrop_bounds(data, cmd);
            if (blur_dirty || cmd.blur != blur_radius || !internal::rect_inside(panel, blur_exact)) {
                blur_dirty = true;
                if (!panel.empty() && s.run_blur(host_fbo, target_w, target_h, cmd.blur, panel, blur_vw, blur_vh, blur_exact)) {
                    blur_dirty  = false;
                    blur_radius = cmd.blur;
                    s.bind_ui_uniforms(s.prog_main, uc, data.display_size.y);
                    mode        = ps_mode::main;
                    bound       = 0;
                    has_scissor = false;
                }
            }
            if (!blur_dirty) { want = ps_mode::backdrop; }
        }

        if (want == ps_mode::image) {
            const GLuint tex = cmd.texture <= s.textures.size() ? s.textures[cmd.texture - 1].tex : 0;
            if (tex == 0) { continue; }
            if (mode != ps_mode::image) { s.bind_ui_uniforms(s.prog_image, uc, data.display_size.y); }
            if (mode != ps_mode::image || bound != cmd.texture) {
                gl.ActiveTexture(kGL_TEXTURE0 + 2);
                gl.BindTexture(kGL_TEXTURE_2D, tex);
            }
            bound = cmd.texture;
        } else if (want == ps_mode::backdrop) {
            if (mode != ps_mode::backdrop) {
                s.bind_ui_uniforms(s.prog_backdrop, uc, data.display_size.y);
                gl.ActiveTexture(kGL_TEXTURE0 + 3);
                gl.BindTexture(kGL_TEXTURE_2D, s.blur_tex[0]);
                impl::blur_constants bc;
                bc.bp[0] = data.display_size.x;
                bc.bp[1] = data.display_size.y;
                bc.bp[2] = static_cast<float>(blur_vw) / static_cast<float>(s.blur_w);
                bc.bp[3] = static_cast<float>(blur_vh) / static_cast<float>(s.blur_h);
                gl.Uniform4fv(s.prog_backdrop.u_bp, 5, bc.bp);
            }
            bound = 0;
        } else if (mode != ps_mode::main) {
            s.bind_ui_uniforms(s.prog_main, uc, data.display_size.y);
            bound = 0;
        }
        if (want != ps_mode::backdrop) { blur_dirty = true; }
        mode = want;

        const GLint sx = left;
        const GLint sy = static_cast<GLint>(target_h) - bottom;
        const GLint sw = right - left;
        const GLint sh = bottom - top;
        if (!has_scissor || sx != bound_sc[0] || sy != bound_sc[1] || sw != bound_sc[2] || sh != bound_sc[3]) {
            gl.Scissor(sx, sy, sw, sh);
            bound_sc[0] = sx; bound_sc[1] = sy; bound_sc[2] = sw; bound_sc[3] = sh;
            has_scissor = true;
        }
        const auto idx_byte_offset = static_cast<std::uintptr_t>(cmd.idx_offset) * sizeof(index_t);
        gl.DrawElementsBaseVertex(kGL_TRIANGLES, static_cast<GLsizei>(cmd.idx_count), kGL_UNSIGNED_SHORT,
                                  reinterpret_cast<const void*>(idx_byte_offset), static_cast<GLint>(cmd.vtx_offset));
    }
}

} // namespace strata
