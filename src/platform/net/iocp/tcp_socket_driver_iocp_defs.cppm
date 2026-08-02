//
// Created by sexey on 27.07.2026.
//
module;
#include <etl/span.h>

export module net.tcp_socket_driver.iocp:defs;

import reactor;

namespace etsl
{
    constexpr int32_t EXPLICIT_DISPOSE = -1;

    constexpr int32_t INVALID_CACHE_VALUE = 0xBADC0DE;

    enum class tcp_socket_state_e : uint8_t
    {
        NONE,
        CONNECTING,
        CONNECTED,
        DISPOSING,
    };

    struct send_operation_s : C_Reactor::operation_t
    {
        etl::span<uint8_t> content;
        uint32_t transferred;
    };
}
