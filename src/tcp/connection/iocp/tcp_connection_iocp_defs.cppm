//
// Created by sexey on 27.07.2026.
//
module;
#include <etl/span.h>

export module etsl.tcp.connection:defs_iocp;

namespace etsl
{
    constexpr int32_t EXPLICIT_DISPOSE = -1;

    constexpr int32_t INVALID_CACHE_VALUE = 0xBADC0DE;

    enum class tcp_connection_state_e : uint8_t
    {
        NONE,
        CONNECTING,
        CONNECTED,
        DISPOSING,
    };
}
