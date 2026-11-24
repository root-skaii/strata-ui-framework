// Offscreen OpenGL 3.3+ renderer verification: SDF shapes, textures, backdrop blur, hostile GL state restore,
// and output encoding. Creates a hidden Win32 WGL context and draws into an RGBA8 FBO.

#include <strata/backend/opengl.hpp>
#include <strata/context.hpp>

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

constexpr int kWGL_CONTEXT_MAJOR_VERSION_ARB    = 0x2091;
constexpr int kWGL_CONTEXT_MINOR_VERSION_ARB    = 0x2092;
constexpr int kWGL_CONTEXT_PROFILE_MASK_ARB     = 0x9126;
constexpr int kWGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;

constexpr GLenum kGL_FRAMEBUFFER          = 0x8D40;
constexpr GLenum kGL_COLOR_ATTACHMENT0    = 0x8CE0;
constexpr GLenum kGL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum kGL_CLAMP_TO_EDGE        = 0x812F;
constexpr GLenum kGL_BLEND_SRC_RGB        = 0x80C9;

using wgl_create_ctx_fn   = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using gl_gen_fb_fn        = void(APIENTRY*)(GLsizei, GLuint*);
using gl_delete_fb_fn     = void(APIENTRY*)(GLsizei, const GLuint*);
using gl_bind_fb_fn       = void(APIENTRY*)(GLenum, GLuint);
using gl_fb_tex2d_fn      = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using gl_check_fb_fn      = GLenum(APIENTRY*)(GLenum);

struct gl_host {
    HWND  hwnd{};
    HDC   dc{};
    HGLRC rc{};
    gl_gen_fb_fn    gen_fb{};
    gl_delete_fb_fn del_fb{};
    gl_bind_fb_fn   bind_fb{};
    gl_fb_tex2d_fn  fb_tex2d{};
    gl_check_fb_fn  check_fb{};
    GLuint fbo{};
    GLuint color_tex{};
    int width{256};
    int height{256};

    ~gl_host()
    {
        if (dc != nullptr && rc != nullptr) {
            ::wglMakeCurrent(dc, rc);
            if (fbo != 0 && del_fb != nullptr) { del_fb(1, &fbo); }
            if (color_tex != 0) { ::glDeleteTextures(1, &color_tex); }
            ::wglMakeCurrent(nullptr, nullptr);
            ::wglDeleteContext(rc);
        }
        if (hwnd != nullptr && dc != nullptr) { ::ReleaseDC(hwnd, dc); }
        if (hwnd != nullptr) { ::DestroyWindow(hwnd); }
    }

    [[nodiscard]] bool init()
    {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = ::DefWindowProcW;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"strata_render_test_gl";
        ::RegisterClassExW(&wc);
        hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
        if (hwnd == nullptr) { return false; }
        dc = ::GetDC(hwnd);
        if (dc == nullptr) { return false; }

        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize      = sizeof pfd;
        pfd.nVersion   = 1;
        pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        const int pf = ::ChoosePixelFormat(dc, &pfd);
        if (pf == 0 || !::SetPixelFormat(dc, pf, &pfd)) { return false; }

        HGLRC temp = ::wglCreateContext(dc);
        if (temp == nullptr || !::wglMakeCurrent(dc, temp)) { return false; }
        auto create_ctx = reinterpret_cast<wgl_create_ctx_fn>(reinterpret_cast<void*>(::wglGetProcAddress("wglCreateContextAttribsARB")));
        if (create_ctx != nullptr) {
            const int attribs[] = {
                kWGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                kWGL_CONTEXT_MINOR_VERSION_ARB, 3,
                kWGL_CONTEXT_PROFILE_MASK_ARB,  kWGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                0,
            };
            rc = create_ctx(dc, nullptr, attribs);
        }
        ::wglMakeCurrent(nullptr, nullptr);
        ::wglDeleteContext(temp);
        if (rc == nullptr) { rc = ::wglCreateContext(dc); }
        if (rc == nullptr || !::wglMakeCurrent(dc, rc)) { return false; }

        gen_fb   = reinterpret_cast<gl_gen_fb_fn>(reinterpret_cast<void*>(::wglGetProcAddress("glGenFramebuffers")));
        del_fb   = reinterpret_cast<gl_delete_fb_fn>(reinterpret_cast<void*>(::wglGetProcAddress("glDeleteFramebuffers")));
        bind_fb  = reinterpret_cast<gl_bind_fb_fn>(reinterpret_cast<void*>(::wglGetProcAddress("glBindFramebuffer")));
        fb_tex2d = reinterpret_cast<gl_fb_tex2d_fn>(reinterpret_cast<void*>(::wglGetProcAddress("glFramebufferTexture2D")));
        check_fb = reinterpret_cast<gl_check_fb_fn>(reinterpret_cast<void*>(::wglGetProcAddress("glCheckFramebufferStatus")));
        if (!gen_fb || !del_fb || !bind_fb || !fb_tex2d || !check_fb) { return false; }

        ::glGenTextures(1, &color_tex);
        ::glBindTexture(GL_TEXTURE_2D, color_tex);
        ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGL_CLAMP_TO_EDGE);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGL_CLAMP_TO_EDGE);
        ::glBindTexture(GL_TEXTURE_2D, 0);

        gen_fb(1, &fbo);
        bind_fb(kGL_FRAMEBUFFER, fbo);
        fb_tex2d(kGL_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
        return check_fb(kGL_FRAMEBUFFER) == kGL_FRAMEBUFFER_COMPLETE;
    }

    std::vector<strata::u8> read_top_down() const
    {
        bind_fb(kGL_FRAMEBUFFER, fbo);
        std::vector<strata::u8> raw(static_cast<std::size_t>(width) * height * 4);
        ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
        ::glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        std::vector<strata::u8> flipped(raw.size());
        const std::size_t row = static_cast<std::size_t>(width) * 4;
        for (int y = 0; y < height; ++y) {
            std::memcpy(flipped.data() + static_cast<std::size_t>(y) * row,
                        raw.data() + static_cast<std::size_t>(height - 1 - y) * row, row);
        }
        return flipped;
    }
};

std::array<strata::u8, 4> pixel_at(const std::vector<strata::u8>& px, int w, int x, int y)
{
    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

} // namespace

int main()
{
    gl_host host;
    if (!host.init()) {
        std::fprintf(stderr, "render_test_gl: failed to create OpenGL 3.3 context / FBO\n");
        return 1;
    }

    auto created = strata::context::create();
    CHECK(created.has_value());
    if (!created) { return 1; }
    strata::context ui = std::move(*created);

    strata::gl_renderer ren;
    CHECK(ren.create(ui.font()));
    if (!ren.valid()) { return 1; }

    // 1. SDF shape + texture + update_texture
    std::array<strata::u8, 16 * 16 * 4> tex_px{};
    for (std::size_t i = 0; i < tex_px.size(); i += 4) {
        tex_px[i] = 40; tex_px[i + 1] = 220; tex_px[i + 2] = 90; tex_px[i + 3] = 255;
    }
    strata::texture_desc td;
    td.width = 16; td.height = 16; td.updatable = true;
    const strata::texture_id tid = ren.create_texture(td, tex_px);
    CHECK(tid != 0);
    std::array<strata::u8, 16 * 16 * 4> tex_px2{};
    for (std::size_t i = 0; i < tex_px2.size(); i += 4) {
        tex_px2[i] = 230; tex_px2[i + 1] = 70; tex_px2[i + 2] = 40; tex_px2[i + 3] = 255;
    }
    CHECK(ren.update_texture(tid, 0, 0, 16, 16, tex_px2));

    auto run_scene = [&](bool with_backdrop) {
        strata::input_state in;
        in.display_size = {static_cast<float>(host.width), static_cast<float>(host.height)};
        ui.begin_frame(std::move(in));
        auto& dl = ui.draw();
        dl.rect_filled({{16, 16}, {112, 112}}, strata::color{60, 120, 220, 255}, 8.0f);
        dl.image({{140, 16}, {220, 96}}, tid);
        if (with_backdrop) {
            dl.backdrop({{20, 140}, {220, 230}}, 10.0f, strata::color{255, 255, 255, 60});
        }
        ui.end_frame();
    };

    host.bind_fb(kGL_FRAMEBUFFER, host.fbo);
    ::glViewport(0, 0, host.width, host.height);
    ::glDisable(GL_SCISSOR_TEST);
    ::glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    ::glClear(GL_COLOR_BUFFER_BIT);
    run_scene(true);

    // 2. Hostile GL state preservation
    ren.set_state_restore(true);
    ::glEnable(GL_DEPTH_TEST);
    ::glEnable(GL_CULL_FACE);
    ::glEnable(GL_SCISSOR_TEST);
    ::glScissor(1, 2, 33, 44);
    ::glViewport(3, 4, 200, 180);
    ::glBlendFunc(GL_ONE, GL_ZERO);

    ren.render(ui.render_data());

    CHECK(::glIsEnabled(GL_DEPTH_TEST) == GL_TRUE);
    CHECK(::glIsEnabled(GL_CULL_FACE) == GL_TRUE);
    CHECK(::glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE);
    GLint sc[4]{};
    ::glGetIntegerv(GL_SCISSOR_BOX, sc);
    CHECK(sc[0] == 1 && sc[1] == 2 && sc[2] == 33 && sc[3] == 44);
    GLint vp[4]{};
    ::glGetIntegerv(GL_VIEWPORT, vp);
    CHECK(vp[0] == 3 && vp[1] == 4 && vp[2] == 200 && vp[3] == 180);
    GLint src_rgb = 0;
    ::glGetIntegerv(kGL_BLEND_SRC_RGB, &src_rgb);
    CHECK(src_rgb == GL_ONE);

    const auto px = host.read_top_down();
    const auto bg   = pixel_at(px, host.width, 4, 4);
    const auto rect = pixel_at(px, host.width, 64, 64);
    const auto img  = pixel_at(px, host.width, 180, 56);
    const auto blur = pixel_at(px, host.width, 120, 185);
    CHECK(bg[0] < 25 && bg[1] < 25 && bg[2] < 30);
    CHECK(rect[2] > 190 && rect[0] < 90);
    CHECK(img[0] > 200 && img[1] < 100 && img[2] < 70);
    CHECK(blur[0] > bg[0] + 25);

    // 3. Linear vs sRGB output encoding shifts midtones
    ren.set_output({.space = strata::output_space::srgb_view});
    ::glDisable(GL_SCISSOR_TEST);
    ::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    ::glClear(GL_COLOR_BUFFER_BIT);
    run_scene(false);
    ren.render(ui.render_data());
    const auto px_lin = host.read_top_down();
    const auto rect_lin = pixel_at(px_lin, host.width, 64, 64);
    CHECK(rect_lin[2] < rect[2] - 20); // sRGB->linear darkens mid-blue channel

    ren.destroy_texture(tid);
    ren.destroy();

    if (g_failures == 0) { std::fprintf(stderr, "render_test_gl: ok\n"); }
    return g_failures == 0 ? 0 : 1;
}
