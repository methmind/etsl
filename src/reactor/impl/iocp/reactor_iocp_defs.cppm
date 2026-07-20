//
// Created by sexey on 19.07.2026.
//
module;
#include <cstdint>
#include <type_traits>
#include <winsock2.h>

export module reactor.iocp:defs;

import socket.types;
import socket.events;

namespace etsl
{
    struct reg_iocp_s : WSAOVERLAPPED // NOLINT(*-pro-type-member-init)
    {
        bool is_closing{false};
        socket_t fd{INVALID_SOCKET};
        socket_event_callback_t cb{};
    };

    struct task_node_iocp_s
    {
        void(*execute)(task_node_iocp_s*) noexcept = nullptr;
        task_node_iocp_s* next{};
    };

    enum class iocp_code_e : uint8_t
    {
        SHUTDOWN = 0,
        TASK,
        IO
    };

    static_assert(std::is_base_of_v<WSAOVERLAPPED, reg_iocp_s>, "reg_iocp_s must inherit from WSAOVERLAPPED!");
}
