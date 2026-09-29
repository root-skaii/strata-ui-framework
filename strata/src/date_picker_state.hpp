#pragma once

// internal: the date picker's shown month and owning popup, behind context::impl::date_. context_datetime.cpp
// owns the logic built on this -- the only file that touches it.

#include "strata/context.hpp"

namespace strata::internal {

struct date_picker_state {
    id  cal_key_{};
    i32 cal_year_{};
    i32 cal_month_{};
};

} // namespace strata::internal
