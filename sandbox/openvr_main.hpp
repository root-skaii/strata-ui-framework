#pragma once

#include <strata/types.hpp>

#include <string>

// standalone steamvr overlay loop: no window, no message pump, and unlike xr_main.cpp no wait/begin/end frame
// cycle either -- IVROverlay is fed a texture whenever the app feels like it, so this just loops at a fixed rate.
// see strata/include/strata/backend/openvr.hpp.
[[nodiscard]] int run_openvr_sandbox(const std::string& face, float size, int theme_index,
                                     strata::u32 panel_width, strata::u32 panel_height);
