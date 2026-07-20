//
// Created by sexey on 20.07.2026.
//
module;
#include <cassert>
#include <cstdint>

#include <etl/algorithm.h>
#include <etl/chrono.h>

module timer.bucket;

namespace etsl
{
    void C_TimerBucket::add(C_Timer& timer) noexcept
    {
        assert(!this->bucket_.contains_node(timer) && "You cannot add the same timer more than once!");

        this->bucket_.insert(etl::find_if(this->bucket_.begin(), this->bucket_.end(),
            [&timer](const C_Timer& val) {
                return timer.deadline().time_since_epoch() < val.deadline().time_since_epoch();
            }
        ), timer);
    }

    void C_TimerBucket::remove(C_Timer& timer) noexcept
    {
        assert(this->bucket_.contains_node(timer) && "You cannot delete a timer that does not exist!");

        const auto it = etl::find_if(this->bucket_.begin(), this->bucket_.end(),
            [&timer](const C_Timer& val) {
                return &val == &timer;
            }
        );

        assert(it != this->bucket_.end());
        this->bucket_.erase(it);
    }

    uint32_t C_TimerBucket::nextTimeout(const time_point_t& now) const noexcept
    {
        if (this->bucket_.empty()) {
            return UINT32_MAX; // Win32 INFINITE: wait until a completion or an addTimer() wake-up.
        }

        const auto diff = this->bucket_.front().deadline().time_since_epoch() - now.time_since_epoch();
        if (diff <= etl::chrono::duration<int32_t, etl::milli>::zero()) {
            return 0;
        }

        return static_cast<uint32_t>(etl::chrono::duration_cast<etl::chrono::milliseconds>(diff).count());
    }

    C_Timer* C_TimerBucket::pop(const time_point_t& now) noexcept
    {
        if (this->bucket_.empty()) {
            return nullptr;
        }

        const auto first = &this->bucket_.front();
        if (first->deadline().time_since_epoch() > now.time_since_epoch()) {
            return nullptr;
        }

        this->bucket_.pop_front();
        return first;
    }
}
