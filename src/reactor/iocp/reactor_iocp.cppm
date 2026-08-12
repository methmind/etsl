//
// Created by sexey on 23.05.2026.
//
module;
#include <windows.h>
#include <etl/expected.h>
#include <etl/intrusive_list.h>

export module reactor:iocp;

import net;
import :iocp_defs;

import timer;
import timer.bucket;

export namespace etsl
{
    class C_ReactorIOCP
    {
    public:
        using operation_t = operation_iocp_s;
        using task_t = task_iocp_s;
        using dispose_operation_t = dispose_operation_iocp_s;

        ~C_ReactorIOCP() noexcept;
        C_ReactorIOCP() noexcept : halt_(false), iocp_(nullptr) {}

        [[nodiscard]] etl::expected<void, int32_t> initialize() noexcept;

        [[nodiscard]] etl::expected<void, int32_t> associate(socket_t fd) noexcept;

        void detach(dispose_operation_t& operation) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> post(task_t& task) noexcept;

        void addTimer(C_Timer& timer) noexcept;

        void removeTimer(C_Timer& timer) noexcept;

        void run() noexcept;

        void shutdown() noexcept;

    private:
        bool halt_;
        HANDLE iocp_;

        C_TimerBucket timerBucket_;
        etl::intrusive_list<dispose_operation_t, etl::bidirectional_link<0>> disposable_;
    };
}
