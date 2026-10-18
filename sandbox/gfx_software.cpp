// gfx_host over strata::software_renderer: no device, the finished frame is blitted with GDI

#include "gfx_host.hpp"

#include <strata/backend/software.hpp>

#include <windows.h>

namespace {

class software_host final : public gfx_host {
public:
    bool init(HWND__* hwnd, strata::u32 width, strata::u32 height, bool, const strata::font_atlas& atlas) override
    {
        hwnd_ = hwnd;
        if (!renderer_.create(atlas)) { return false; }
        renderer_.resize(width, height);
        return true;
    }

    void resize(strata::u32 width, strata::u32 height) override { renderer_.resize(width, height); }

    void render(const strata::draw_data& ui, strata::color clear) override
    {
        renderer_.clear(clear);
        renderer_.render(ui);
        const strata::u32 w = renderer_.width();
        const strata::u32 h = renderer_.height();
        if (w == 0 || h == 0) { return; }

        const auto src = renderer_.pixels();
        bgra_.resize(src.size());
        for (std::size_t i = 0; i + 3 < src.size(); i += 4) {
            bgra_[i] = src[i + 2]; bgra_[i + 1] = src[i + 1]; bgra_[i + 2] = src[i]; bgra_[i + 3] = 255;
        }
        if (capture_) {
            captured_.resize(src.size());
            (void)renderer_.copy_straight(captured_);
            capture_w_ = w; capture_h_ = h;
            have_capture_ = true;
            capture_ = false;
        }
        if (hwnd_ != nullptr) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth       = static_cast<LONG>(w);
            bi.bmiHeader.biHeight      = -static_cast<LONG>(h);
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            if (HDC dc = ::GetDC(hwnd_)) {
                ::SetDIBitsToDevice(dc, 0, 0, w, h, 0, 0, 0, h, bgra_.data(), &bi, DIB_RGB_COLORS);
                ::ReleaseDC(hwnd_, dc);
            }
        }
    }

    strata::texture_id create_texture(strata::u32 w, strata::u32 h, std::span<const strata::u8> rgba) override
    {
        return renderer_.create_texture(w, h, rgba);
    }
    strata::texture_id create_texture(const strata::texture_desc& desc, std::span<const strata::u8> pixels) override
    {
        return renderer_.create_texture(desc, pixels);
    }
    bool update_texture(strata::texture_id id, strata::u32 x, strata::u32 y, strata::u32 w, strata::u32 h,
                        std::span<const strata::u8> pixels) override
    {
        return renderer_.update_texture(id, x, y, w, h, pixels);
    }
    const char* name() const noexcept override { return "software"; }
    bool update_atlas(const strata::font_atlas& atlas) override { return renderer_.update_atlas(atlas); }

    void request_capture() override { capture_ = true; have_capture_ = false; }
    bool take_capture(std::vector<strata::u8>& rgba, strata::u32& width, strata::u32& height) override
    {
        if (!have_capture_) { return false; }
        rgba   = std::move(captured_);
        width  = capture_w_;
        height = capture_h_;
        have_capture_ = false;
        return true;
    }

private:
    strata::software_renderer renderer_;
    HWND__*                   hwnd_{};
    std::vector<strata::u8>   bgra_;
    std::vector<strata::u8>   captured_;
    strata::u32               capture_w_{};
    strata::u32               capture_h_{};
    bool                      capture_{};
    bool                      have_capture_{};
};

} // namespace

std::unique_ptr<gfx_host> make_software_host() { return std::make_unique<software_host>(); }
