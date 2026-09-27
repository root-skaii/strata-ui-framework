#pragma once

// umbrella header: core ui (renderer-agnostic). backends: strata/backend/d3d11.hpp, strata/backend/d3d12.hpp;
// win32 input: strata/platform/win32.hpp.

#include "strata/bidi.hpp"
#include "strata/config.hpp"
#include "strata/context.hpp"
#include "strata/datetime.hpp"
#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/icons.hpp"
#include "strata/keybinds.hpp"
#include "strata/log.hpp"
#include "strata/log_queue.hpp"
#include "strata/texture.hpp"
#include "strata/themes.hpp"
#include "strata/types.hpp"
#include "strata/vmem.hpp"
