//
// Created by sexey on 23.05.2026.
//
module;
#include <winsock2.h>
#include <windows.h>
#include <etl/atomic.h>
#include <etl/expected.h>
#include <etl/intrusive_list.h>

export module etsl.reactor:iocp;

import etsl.net;
import etsl.timer;

export import :iocp_defs;

export namespace etsl
{
    class C_ReactorIOCP
    {
    public:
        using operation_t = C_OperationIOCP;
        using dispose_operation_t = dispose_operation_iocp_s;

        ~C_ReactorIOCP() noexcept;
        C_ReactorIOCP() noexcept : halt_(false), iocp_(nullptr) {}

        [[nodiscard]] etl::expected<void, int32_t> initialize() noexcept;

        [[nodiscard]] etl::expected<void, int32_t> associate(socket_t fd) noexcept;

        void detach(dispose_operation_t& operation) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> post(operation_t& task) noexcept;

        void schedule(C_Timer& timer, const timer_duration_t& duration) noexcept;

        void unschedule(C_Timer& timer) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> run() noexcept;

        [[nodiscard]] etl::expected<void, int32_t> shutdown() noexcept;

    private:
        [[nodiscard]] static int32_t TranslateError(socket_t fd, WSAOVERLAPPED* operation) noexcept;

        [[nodiscard]] uint32_t nextTimeout() const noexcept;

        [[nodiscard]] const dispose_operation_t* popDisposable() noexcept;

        [[nodiscard]] static etl::expected<void, int32_t> ProceedOperations(HANDLE iocp, uint32_t timeout) noexcept;

        etl::atomic<bool> halt_;
        HANDLE iocp_;

        C_TimerQueue timerQueue_;
        etl::intrusive_list<dispose_operation_t, etl::bidirectional_link<0>> disposable_;
    };
}
