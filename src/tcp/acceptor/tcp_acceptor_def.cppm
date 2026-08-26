//
// Created by sexey on 23.08.2026.
//
module;
#include <cstdint>

export module etsl.tcp.acceptor:defs;

import etsl.net;
import etsl.reactor;

export namespace etsl
{
    constexpr auto MICROSLOP_MAGIC_NUMBER = 16;

    constexpr auto ACCEPT_BUFFER_SIZE = sizeof(os_sockaddr_in_t) + MICROSLOP_MAGIC_NUMBER;

    struct accept_operation_s : C_Reactor::operation_t
    {
#if defined(_WIN32)
        C_Socket fd{INVALID_SOCKET};
        uint8_t buffer[ACCEPT_BUFFER_SIZE * 2]{};
#elif defined(__linux__)
#error "Linux reactor is not implemented yet"
#else
#error "Unsupported platform"
#endif
    };

    using accept_operation_t = accept_operation_s;
}