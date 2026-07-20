//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/intrusive_list.h>

export module timer.bucket;

import timer;

export namespace etsl
{
    class C_TimerBucket
    {
    public:
        ~C_TimerBucket() noexcept = default;
        C_TimerBucket() noexcept = default;

        void add(C_Timer& timer) noexcept;

        void remove(C_Timer& timer) noexcept;

        [[nodiscard]] uint32_t nextTimeout(const time_point_t& now) const noexcept;

        C_Timer* pop(const time_point_t& now) noexcept;

    private:
        etl::intrusive_list<C_Timer, etl::bidirectional_link<0>> bucket_;
    };
}
