//
// Created by sexey on 28.08.2026.
//
module;
#include <concepts>
#include <cstdint>
#include <utility>

export module etsl.tcp.acceptor:delegate;

import etsl.net;

namespace etsl
{
    template<typename T>
    concept TCPAcceptorDelegate = requires(T t, C_Socket fd, const C_Address& remoteAddr, int32_t error)
    {
        { t.onIncoming(std::move(fd), remoteAddr) } noexcept -> std::same_as<void>;

        { t.onError(error) } noexcept -> std::same_as<void>;

        { t.onDisposed(error) } noexcept -> std::same_as<void>;
    };
}
