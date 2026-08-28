//
// Created by sexey on 20.07.2026.
//
module;
#include <winsock2.h>
#include <ws2tcpip.h>

#include <etl/expected.h>
#include <etl/endianness.h>

module etsl.net;

import :ops;

namespace etsl
{
    etl::expected<uint32_t, int32_t> ParseIPV4(const char* ip, const uint16_t port, os_sockaddr_storage_t& storage) noexcept
    {
        if (!ip) {
            return etl::unexpected(WSAEINVAL);
        }

        memset(&storage, 0, sizeof(storage));

        auto& sa = reinterpret_cast<os_sockaddr_in_t&>(storage);
        sa.sin_family = AF_INET;
        sa.sin_port = etl::hton<uint16_t>(port);
        if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
            return etl::unexpected(WSAGetLastError());
        }

        return sizeof(os_sockaddr_in_t);
    }

    bool CloseSocket(socket_t fd) noexcept
    {
        return closesocket(fd) != SOCKET_ERROR;
    }

    bool Shutdown(socket_t fd, socket_shutdown_e mode) noexcept
    {
        return shutdown(fd, static_cast<int32_t>(mode)) != SOCKET_ERROR;
    }

    etl::expected<void*, int32_t> GetExtensionFunction(socket_t fd, GUID guid) noexcept
    {
        DWORD bytes = 0;
        void* fnPtr = nullptr;

        if (WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
            &fnPtr, sizeof(fnPtr), &bytes, nullptr,
            nullptr) != ERROR_SUCCESS) {
            return etl::unexpected(WSAGetLastError());
        }

        return fnPtr;
    }
}
