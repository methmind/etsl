//
// Created by sexey on 20.07.2026.
//
module;
#if defined(_WIN32)
#include <winsock2.h>
#elif defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#else
#error "Unsupported platform"
#endif

export module etsl.net:defs;

export namespace etsl
{
    using os_sockaddr_storage_t = sockaddr_storage;

    using os_sockaddr_in_t = sockaddr_in;

    using os_sockaddr = sockaddr;

#if defined(_WIN32)
#undef INVALID_SOCKET
#undef SOCKET_ERROR
    using socket_t = SOCKET;

    constexpr socket_t INVALID_SOCKET = ~0;

    constexpr auto SOCKET_ERROR = -1;
#elif defined(__linux__)
    using socket_t = int;

    constexpr socket_t INVALID_SOCKET = -1;

    constexpr auto SOCKET_ERROR = -1;
#endif
}