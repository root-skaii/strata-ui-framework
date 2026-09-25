#pragma once

// umbrella header: core ui (renderer-agnostic). backends live in
// strata/backend/d3d11.hpp and strata/backend/d3d12.hpp,
// the win32 input glue in strata/platform/win32.hpp.

#include "strata/bidi.hpp"
#include "strata/config.hpp"
#include "strata/context.hpp"
#include "strata/datetime.hpp"
#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/icons.hpp"
#include "strata/keybinds.hpp"
#include "strata/log.hpp"
#include "strata/texture.hpp"
#include "strata/themes.hpp"
#include "strata/types.hpp"
#include "strata/vmem.hpp"
