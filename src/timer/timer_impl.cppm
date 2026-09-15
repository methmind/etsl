//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/intrusive_list.h>
#include <util/noncopyable.h>

export module etsl.timer:impl;

import :defs;

namespace etsl
{
    struct timer_node_s : etl::bidirectional_link<0> {};

    export class C_Timer : private timer_node_s
    {
    public:
        ~C_Timer() noexcept { assert(!is_linked() && "UAF error caught!"); }

        explicit C_Timer(const timer_callback_t& callback) noexcept : callback_(callback) {}

        ETSL_NON_COPYABLE_NON_MOVABLE(C_Timer);

        void execute() const noexcept
        {
            assert(this->callback_ && "Timer callback should not be a null!");
            this->callback_();
        }

        [[nodiscard]] bool isArmed() const noexcept { return is_linked(); }

        [[nodiscard]] const time_point_t& deadline() const noexcept { return this->deadline_; }

    private:
        friend class C_TimerQueue;

        time_point_t deadline_;
        timer_callback_t callback_;
    };
}