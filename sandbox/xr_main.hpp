#pragma once

#include <strata/types.hpp>

#include <string>

// standalone vr loop: no window, no message pump -- separate from gfx_host / main.cpp's windowed path entirely,
// since openxr has its own frame timing (xrWaitFrame/xrBeginFrame/xrEndFrame) and event polling that doesn't fit
// the "one swap chain, one Present" shape gfx_host assumes. see strata/include/strata/backend/openxr.hpp.
[[nodiscard]] int run_xr_sandbox(const std::string& face, float size, int theme_index,
                                 strata::u32 panel_width, strata::u32 panel_height);
