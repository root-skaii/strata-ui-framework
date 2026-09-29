#pragma once

#include "strata/types.hpp"

namespace strata {

// right-handed, metres. shared by the openxr and openvr backends to place a panel in the tracking volume and
// describe controller poses; strata's own 2d ui code never touches these.
struct vr_vec3 { f32 x{}, y{}, z{}; };
struct vr_quat { f32 x{}, y{}, z{}, w{1.0f}; };
struct vr_pose { vr_vec3 position{}; vr_quat orientation{}; };

} // namespace strata
