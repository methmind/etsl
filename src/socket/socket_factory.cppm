//
// Created by sexey on 29.05.2026.
//
module;
#include <winsock2.h>

#include "etl/expected.h"

export module socket.factory;
import socket.raii;
import socket.types;

export namespace etsl
{
    etl::expected<C_Socket, int32_t> CreateSocket(const int32_t type = SOCK_STREAM) noexcept
    {
        auto sock = C_Socket(socket(AF_INET, type, 0));
        if (!sock) {
            return etl::unexpected(WSAGetLastError());
        }

        u_long value = 1;
        if (ioctlsocket(sock.get(), FIONBIO, &value) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        if (type == SOCK_STREAM) {
            if (setsockopt(sock.get(), IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&value), sizeof(value)) == SOCKET_ERROR) {
                return etl::unexpected(WSAGetLastError());
            }
        }

        return std::move(sock);
    }
}
