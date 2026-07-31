//
// Created by sexey on 27.07.2026.
//
module;
#include <cstdint>

export module net.tcp_socket_driver.iocp:defs;

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
}
