//
// Created by sexey on 05.08.2026.
//
module;
#include <concepts>
#include <cstdint>

module etsl.tcp.connection:delegate;

import :defs;

namespace etsl
{
    template<typename T>
    concept TCPConnectionDelegate = requires(T t, send_operation_t& sendOperation, int32_t error)
    {
        { t.onReadyRead() } noexcept -> std::same_as<void>;

        { t.onCommit(sendOperation, error) } noexcept -> std::same_as<void>;

        { t.onConnect(error) } noexcept -> std::same_as<void>;

        { t.onDisconnect(error) } noexcept -> std::same_as<void>;

        { t.onDisposed() } noexcept -> std::same_as<void>;
    };
}
