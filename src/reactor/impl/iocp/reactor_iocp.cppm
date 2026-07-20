//
// Created by sexey on 23.05.2026.
//
module;
#include <windows.h>
#include "etl/atomic.h"
#include "etl/expected.h"

export module reactor.iocp;

import socket.types;
import socket.events;
import :defs;

import timer;
import timer.bucket;

export namespace etsl
{
    class C_ReactorIOCP
    {
    public:
        using reg_info_t = reg_iocp_s;
        using task_t = task_node_iocp_s;

        ~C_ReactorIOCP() noexcept;
        C_ReactorIOCP() noexcept : halt_(false), iocp_(nullptr) {}

        [[nodiscard]] etl::expected<void, int32_t> initialize() noexcept;

        [[nodiscard]] etl::expected<void, int32_t> attach(socket_t fd, const socket_event_callback_t& cb, reg_info_t& reg) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> detach(socket_t fd, reg_info_t& reg) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> post(task_t* task) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> addTimer(C_Timer& timer) noexcept;

        void removeTimer(C_Timer& timer) noexcept;

        void run() noexcept;

        void shutdown() noexcept;
    private:
        [[nodiscard]] static etl::expected<void, int32_t> RearmRead(reg_info_t* reg) noexcept;

        etl::atomic<bool> halt_;
        HANDLE iocp_;
        C_TimerBucket timerBucket_;
    };
}
