//
// Created by sexey on 18.07.2026.
//
module;

#if defined(_WIN32)
#include <winsock2.h>
#elif defined(__linux__)
#include <sys/socket.h>
#else
#error "Unsupported platform"
#endif

#include <cstdint>

export module socket.types;

export namespace etsl
{
#if defined(_WIN32)
    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCKET_VALUE = INVALID_SOCKET;
#elif defined(__linux__)
    using socket_t = int;
    constexpr socket_t INVALID_SOCKET_VALUE = -1;
#endif
}
