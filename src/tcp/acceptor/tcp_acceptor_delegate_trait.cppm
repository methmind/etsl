//
// Created by sexey on 28.08.2026.
//
module;
#include <concepts>
#include <utility>

module etsl.tcp.acceptor:delegate;

import etsl.net;

namespace etsl
{
    template<typename T>
    concept TCPAcceptorDelegate = requires(T t, C_Socket fd)
    {
        { t.onIncoming(std::move(fd)) } noexcept -> std::same_as<void>;

        { t.onDisposed() } noexcept -> std::same_as<void>;
    };
}
