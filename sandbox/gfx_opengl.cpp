#include "gfx_host.hpp"

#include <strata/backend/opengl.hpp>

#include <windows.h>
#include <GL/gl.h>

#include <cstring>
#include <vector>

namespace {

// WGL extension constants for creating an OpenGL 3.3 Core context
constexpr int kWGL_DRAW_TO_WINDOW_ARB             = 0x2001;
constexpr int kWGL_SUPPORT_OPENGL_ARB             = 0x2010;
constexpr int kWGL_DOUBLE_BUFFER_ARB              = 0x2011;
constexpr int kWGL_PIXEL_TYPE_ARB                 = 0x2013;
constexpr int kWGL_TYPE_RGBA_ARB                  = 0x202B;
constexpr int kWGL_COLOR_BITS_ARB                 = 0x2014;
constexpr int kWGL_ALPHA_BITS_ARB                 = 0x201B;
constexpr int kWGL_CONTEXT_MAJOR_VERSION_ARB      = 0x2091;
constexpr int kWGL_CONTEXT_MINOR_VERSION_ARB      = 0x2092;
constexpr int kWGL_CONTEXT_FLAGS_ARB              = 0x2094;
#ifndef NDEBUG
constexpr int kWGL_CONTEXT_DEBUG_BIT_ARB          = 0x0001;
#endif
constexpr int kWGL_CONTEXT_PROFILE_MASK_ARB       = 0x9126;
constexpr int kWGL_CONTEXT_CORE_PROFILE_BIT_ARB   = 0x0001;

using wgl_choose_pf_fn     = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using wgl_create_ctx_fn    = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using wgl_swap_interval_fn = BOOL(WINAPI*)(int);

class opengl_host final : public gfx_host {
public:
    ~opengl_host() override
    {
        if (hdc_ != nullptr && hglrc_ != nullptr) {
            ::wglMakeCurrent(hdc_, hglrc_);
            ui_.destroy();
            ::wglMakeCurrent(nullptr, nullptr);
            ::wglDeleteContext(hglrc_);
        }
        if (hwnd_ != nullptr && hdc_ != nullptr) {
            ::ReleaseDC(hwnd_, hdc_);
        }
    }

    bool init(HWND__* hwnd, strata::u32 width, strata::u32 height, bool vsync, const strata::font_atlas& atlas) override
    {
        hwnd_   = reinterpret_cast<HWND>(hwnd);
        width_  = width;
        height_ = height;

        // bootstrap WGL extensions via a temporary hidden window
        wgl_choose_pf_fn     wgl_choose_pf     = nullptr;
        wgl_create_ctx_fn    wgl_create_ctx    = nullptr;
        wgl_swap_interval_fn wgl_swap_interval = nullptr;
        {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc   = ::DefWindowProcW;
            wc.hInstance     = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = L"strata_gl_bootstrap";
            ::RegisterClassExW(&wc);
            HWND dummy = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 1, 1,
                                           nullptr, nullptr, wc.hInstance, nullptr);
            if (dummy != nullptr) {
                HDC ddc = ::GetDC(dummy);
                PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1};
                pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
                pfd.iPixelType = PFD_TYPE_RGBA;
                pfd.cColorBits = 32;
                pfd.cAlphaBits = 8;
                const int pf = ::ChoosePixelFormat(ddc, &pfd);
                if (pf != 0 && ::SetPixelFormat(ddc, pf, &pfd)) {
                    if (HGLRC drc = ::wglCreateContext(ddc); drc != nullptr) {
                        ::wglMakeCurrent(ddc, drc);
                        wgl_choose_pf     = reinterpret_cast<wgl_choose_pf_fn>(reinterpret_cast<void*>(::wglGetProcAddress("wglChoosePixelFormatARB")));
                        wgl_create_ctx    = reinterpret_cast<wgl_create_ctx_fn>(reinterpret_cast<void*>(::wglGetProcAddress("wglCreateContextAttribsARB")));
                        wgl_swap_interval = reinterpret_cast<wgl_swap_interval_fn>(reinterpret_cast<void*>(::wglGetProcAddress("wglSwapIntervalEXT")));
                        ::wglMakeCurrent(nullptr, nullptr);
                        ::wglDeleteContext(drc);
                    }
                }
                ::ReleaseDC(dummy, ddc);
                ::DestroyWindow(dummy);
            }
            ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        }

        hdc_ = ::GetDC(hwnd_);
        if (hdc_ == nullptr) { return false; }

        int pf = 0;
        PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1};
        if (wgl_choose_pf != nullptr) {
            const int attribs[] = {
                kWGL_DRAW_TO_WINDOW_ARB, 1,
                kWGL_SUPPORT_OPENGL_ARB, 1,
                kWGL_DOUBLE_BUFFER_ARB,  1,
                kWGL_PIXEL_TYPE_ARB,     kWGL_TYPE_RGBA_ARB,
                kWGL_COLOR_BITS_ARB,     32,
                kWGL_ALPHA_BITS_ARB,     8,
                0,
            };
            UINT count = 0;
            if (wgl_choose_pf(hdc_, attribs, nullptr, 1, &pf, &count) && count > 0) {
                ::DescribePixelFormat(hdc_, pf, sizeof(pfd), &pfd);
            } else {
                pf = 0;
            }
        }
        if (pf == 0) {
            pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            pfd.iPixelType = PFD_TYPE_RGBA;
            pfd.cColorBits = 32;
            pfd.cAlphaBits = 8;
            pf = ::ChoosePixelFormat(hdc_, &pfd);
        }
        if (pf == 0 || !::SetPixelFormat(hdc_, pf, &pfd)) { return false; }

        if (wgl_create_ctx != nullptr) {
            int flags = 0;
#ifndef NDEBUG
            flags |= kWGL_CONTEXT_DEBUG_BIT_ARB;
#endif
            const int ctx_attribs[] = {
                kWGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                kWGL_CONTEXT_MINOR_VERSION_ARB, 3,
                kWGL_CONTEXT_PROFILE_MASK_ARB,  kWGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                kWGL_CONTEXT_FLAGS_ARB,         flags,
                0,
            };
            hglrc_ = wgl_create_ctx(hdc_, nullptr, ctx_attribs);
        }
        if (hglrc_ == nullptr) {
            hglrc_ = ::wglCreateContext(hdc_);
        }
        if (hglrc_ == nullptr || !::wglMakeCurrent(hdc_, hglrc_)) { return false; }

        if (wgl_swap_interval != nullptr) {
            wgl_swap_interval(vsync ? 1 : 0);
        }

        return ui_.create(atlas);
    }

    void resize(strata::u32 width, strata::u32 height) override
    {
        if (width == 0 || height == 0) { return; }
        width_  = width;
        height_ = height;
    }

    void render(const strata::draw_data& ui, strata::color clear) override
    {
        if (hglrc_ == nullptr) { return; }
        ::glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
        ::glDisable(GL_SCISSOR_TEST);
        ::glClearColor(clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, 1.0f);
        ::glClear(GL_COLOR_BUFFER_BIT);
        ui_.render(ui);
        if (capture_requested_) {
            capture_requested_ = false;
            read_back();
        }
        ::SwapBuffers(hdc_);
    }

    strata::texture_id create_texture(strata::u32 width, strata::u32 height, std::span<const strata::u8> rgba) override
    {
        return ui_.create_texture(width, height, rgba);
    }
    strata::texture_id create_texture(const strata::texture_desc& desc, std::span<const strata::u8> pixels) override
    {
        return ui_.create_texture(desc, pixels);
    }
    bool update_texture(strata::texture_id id, strata::u32 x, strata::u32 y, strata::u32 width, strata::u32 height,
                        std::span<const strata::u8> pixels) override
    {
        return ui_.update_texture(id, x, y, width, height, pixels);
    }

    const char* name() const noexcept override { return "opengl 3.3"; }

    bool update_atlas(const strata::font_atlas& atlas) override { return ui_.update_atlas(atlas); }

    void request_capture() override { capture_requested_ = true; }
    bool take_capture(std::vector<strata::u8>& rgba, strata::u32& width, strata::u32& height) override
    {
        if (captured_.empty()) { return false; }
        rgba   = std::move(captured_);
        width  = captured_w_;
        height = captured_h_;
        captured_.clear();
        return true;
    }

private:
    void read_back()
    {
        if (width_ == 0 || height_ == 0) { return; }
        std::vector<strata::u8> raw(static_cast<std::size_t>(width_) * height_ * 4);
        ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
        ::glReadBuffer(GL_BACK);
        ::glReadPixels(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_), GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        captured_w_ = width_;
        captured_h_ = height_;
        captured_.resize(raw.size());
        const std::size_t stride = static_cast<std::size_t>(width_) * 4;
        for (strata::u32 y = 0; y < height_; ++y) {
            std::memcpy(captured_.data() + static_cast<std::size_t>(y) * stride,
                        raw.data() + static_cast<std::size_t>(height_ - 1 - y) * stride, stride);
        }
    }

    HWND                   hwnd_{};
    HDC                    hdc_{};
    HGLRC                  hglrc_{};
    strata::u32            width_{}, height_{};
    strata::gl_renderer    ui_;
    bool                   capture_requested_{};
    std::vector<strata::u8> captured_;
    strata::u32            captured_w_{}, captured_h_{};
};

} // namespace

std::unique_ptr<gfx_host> make_opengl_host()
{
    return std::make_unique<opengl_host>();
}
