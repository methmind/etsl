//
// Created by sexey on 05.08.2026.
//
module;
#include <concepts>

module net.tcp_socket_driver:delegate;

import socket.types;
import :defs;

namespace etsl
{
    template<typename T>
    concept TCPDriverDelegate = requires(T t, socket_t fd, send_operation_s& sendOperation, int32_t error)
    {
        { t.onReadyRead(fd) } noexcept -> std::same_as<void>;

        { t.onCommit(sendOperation, error) } noexcept -> std::same_as<void>;

        { t.onConnect(error) } noexcept -> std::same_as<void>;

        { t.onDisconnect(error) } noexcept -> std::same_as<void>;

        { t.onDisposed() } noexcept -> std::same_as<void>;
    };
}
