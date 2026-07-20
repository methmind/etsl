//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/intrusive_list.h>

export module timer;

export import :types;

export namespace etsl
{
    class C_Timer : public etl::bidirectional_link<0>
    {
    public:
        ~C_Timer() noexcept = default;

        explicit C_Timer(const timer_cb_t& callback) noexcept : callback_(callback) {}

        void execute() const noexcept
        {
            assert(this->callback_ && "Timer callback should not be null!");
            this->callback_();
        }

        void arm(const time_point_t& absoluteDeadline) noexcept
        {
            this->deadline_ = absoluteDeadline;
        }

        [[nodiscard]] const time_point_t& deadline() const noexcept { return this->deadline_; }

    private:
        time_point_t deadline_;
        timer_cb_t callback_;
    };
}