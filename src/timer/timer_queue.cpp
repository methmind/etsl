//
// Created by sexey on 20.07.2026.
//
module;
#include <cassert>
#include <cstdint>

#include <etl/algorithm.h>
#include <etl/chrono.h>

module etsl.timer;

import :queue;

namespace etsl
{
    void C_TimerQueue::schedule(C_Timer& timer, const timer_duration_t& duration) noexcept
    {
        if (timer.is_linked()) {
            assert(contains_node(timer) && "You cannot re-add not associated timer!");
            disconnect_link(&timer);
        }

        timer.deadline_ = steady_clock_t::now() + duration;
        auto it = get_tail();
        while (it != &this->terminal_link && timer.deadline_ < static_cast<C_Timer*>(it)->deadline_) {
            it = it->etl_previous;
        }

        insert_link(it, timer);
    }

    void C_TimerQueue::unschedule(C_Timer& timer) noexcept
    {
        if (!timer.is_linked()) {
            return;
        }

        assert(contains_node(timer) && "You cannot delete unassociated timer!");
        disconnect_link(&timer);
    }

    const time_point_t* C_TimerQueue::nearest() const noexcept
    {
        if (empty()) {
            return nullptr;
        }

        return &static_cast<const C_Timer*>(get_head())->deadline_;
    }

    const C_Timer* C_TimerQueue::pop(const time_point_t& now) noexcept
    {
        if (empty()) {
            return nullptr;
        }

        const auto first = static_cast<C_Timer*>(get_head());
        if (first->deadline_ > now) {
            return nullptr;
        }

        disconnect_link(first);
        return first;
    }
}
