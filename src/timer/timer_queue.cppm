//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/intrusive_list.h>

export module etsl.timer:queue;

import :impl;

export namespace etsl
{
    class C_TimerQueue : private etl::intrusive_list<timer_node_s, etl::bidirectional_link<0>>
    {
    public:
        ~C_TimerQueue() noexcept = default;
        C_TimerQueue() noexcept = default;

        void schedule(C_Timer& timer, const timer_duration_t& duration) noexcept;

        void unschedule(C_Timer& timer) noexcept;

        [[nodiscard]] const time_point_t* nearest() const noexcept;

        [[nodiscard]] C_Timer* pop(const time_point_t& now) noexcept;
    };
}
