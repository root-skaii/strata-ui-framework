#include "internal.hpp"

namespace strata::overlay::detail {

namespace {

// is the monitor the swap chain is on in hdr mode (Windows "Use HDR")
[[nodiscard]] bool output_is_hdr(IDXGISwapChain* sc)
{
    ComPtr<IDXGIOutput> out;
    ComPtr<IDXGIOutput6> out6;
    DXGI_OUTPUT_DESC1 od{};
    return SUCCEEDED(sc->GetContainingOutput(&out)) && SUCCEEDED(out.As(&out6)) && SUCCEEDED(out6->GetDesc1(&od)) &&
           od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

// colour encoding: forced by options, or from the swap chain -- FP16 is scRGB, 10-bit is HDR10 if the game
// declared it (SetColorSpace1) or, if declared before we loaded, the monitor is in hdr mode; else srgb
[[nodiscard]] output_desc detect_output(IDXGISwapChain* sc)
{
    output_desc out;
    out.paper_white_nits = g.opt.hdr_paper_white_nits;
    if (g.opt.output.has_value()) {
        out.space = *g.opt.output;
        return out;
    }
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) { return out; }
    const int declared = g.colorspace_chain.load() == sc ? g.colorspace.load() : -1;
    if (declared == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)        { out.space = output_space::scrgb; }
    else if (declared == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) { out.space = output_space::hdr10; }
    else if (declared >= 0)                                          { out.space = output_space::srgb; }
    else if (d.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)  { out.space = output_space::scrgb; }
    else if (d.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && output_is_hdr(sc)) { out.space = output_space::hdr10; }
    // scRGB on a non-hdr monitor: 1.0 is already display white, so a paper white above 80 nits would blow the ui out
    if (out.space == output_space::scrgb && !output_is_hdr(sc)) { out.paper_white_nits = 80.0f; }
    return out;
}

} // namespace

void apply_output(IDXGISwapChain* sc)
{
    if (!g.output_dirty.exchange(false)) { return; }
    const output_desc out = detect_output(sc);
    g.renderer.set_output(out);
    g.renderer12.set_output(out);
    g.output_now.store(static_cast<int>(out.space));
    log_line("output: %s", out.space == output_space::scrgb ? "scRGB" : out.space == output_space::hdr10 ? "HDR10" : "srgb");
}

} // namespace strata::overlay::detail
