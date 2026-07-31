//
// Created by sexey on 31.07.2026.
//
module;
#include <cstdint>
#include <etl/delegate.h>

export module net.tcp_socket_driver.types;

import reactor;
import socket.types;

export namespace etsl
{
    using on_ready_read_callback_t = etl::delegate<void(socket_t fd)>;

    using on_commit_callback_t = etl::delegate<void(C_Reactor::operation_t& operation, int32_t error)>;

    using on_connect_callback_t = etl::delegate<void(int32_t error)>;

    using on_disconnect_callback_t = etl::delegate<void(int32_t error)>;

    using on_disposed_callback_t = etl::delegate<void()>;

    struct tcp_socket_driver_events_s
    {
        on_ready_read_callback_t onReadyRead;
        on_commit_callback_t onCommit;
        on_connect_callback_t onConnect;
        on_disconnect_callback_t onDisconnect;
        on_disposed_callback_t onDisposed;
    };
}
