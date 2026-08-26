//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/chrono.h>
#include <etl/delegate.h>

export module etsl.timer:defs;

export namespace etsl
{
    using clock_t = etl::chrono::steady_clock;

    using time_point_t = clock_t::time_point;

    using timer_callback_t = etl::delegate<void()>;
}
