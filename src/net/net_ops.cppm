//
// Created by sexey on 20.07.2026.
//
module;
#if defined(_WIN32)
#include <guiddef.h>
#elif defined(__linux__)
#error "Linux tcp socket driver is not implemented yet"
#else
#error "Unsupported platform"
#endif

#include <etl/expected.h>

export module etsl.net:ops;

import :defs;

export namespace etsl
{
    enum class socket_shutdown_e : int32_t
    {
        RECEIVE = 0,
        SEND = 1,
        BOTH = 2
    };

    etl::expected<uint32_t, int32_t> ParseIPV4(const char* ip, uint16_t port, os_sockaddr_storage_t& storage) noexcept;

    bool CloseSocket(socket_t fd) noexcept;

    bool Shutdown(socket_t fd, socket_shutdown_e mode) noexcept;

#if defined(_WIN32)
    etl::expected<void*, int32_t> GetExtensionFunction(socket_t fd, GUID guid) noexcept;
#endif
}
