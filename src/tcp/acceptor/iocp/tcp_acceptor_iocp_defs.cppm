//
// Created by sexey on 28.08.2026.
//
module;
#include <cstdint>
#include <util/noncopyable.h>

export module etsl.tcp.acceptor:defs_iocp;

import etsl.net;
import etsl.reactor;

namespace etsl
{
    constexpr auto MICROSLOP_MAGIC_NUMBER = 16;

    constexpr auto ACCEPT_BUFFER_SIZE = sizeof(os_sockaddr_storage_t) + MICROSLOP_MAGIC_NUMBER;

    constexpr int32_t EXPLICIT_DISPOSE = -1;

    constexpr int32_t INVALID_CACHE_VALUE = 0xBADC0DE;

    enum class tcp_acceptor_state_e : uint8_t
    {
        NONE,
        LISTENING,
        DISPOSING,
    };

    struct accept_operation_s : C_Reactor::operation_t
    {
        C_Socket fd{INVALID_SOCKET};
        uint8_t buffer[ACCEPT_BUFFER_SIZE * 2]{};
    };
}
