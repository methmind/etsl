//
// Created by sexey on 28.08.2026.
//
module;
#include <winsock2.h>

#include <etl/expected.h>

module etsl.net;

import :factory;
import :socket;

namespace etsl
{
    etl::expected<C_Socket, int32_t> CreateSocket() noexcept
    {
        auto sock = C_Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!sock) {
            return etl::unexpected(WSAGetLastError());
        }

        u_long value = 1;
        if (ioctlsocket(sock.get(), FIONBIO, &value) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        if (setsockopt(sock.get(), IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&value), sizeof(value)) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        return std::move(sock);
    }
}
