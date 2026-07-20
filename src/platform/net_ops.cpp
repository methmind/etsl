//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/expected.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#else
#error "Unsupported platform"
#endif
module net.ops;

namespace etsl
{
    etl::expected<size_t, int32_t> ParseIPV4(const char* ip, const uint16_t port, os_sockaddr_storage_t& storage) noexcept
    {
        const auto sa = reinterpret_cast<os_sockaddr_in_t*>(&storage);
        sa->sin_family = AF_INET;
        sa->sin_port = htons(port);

        if (inet_pton(AF_INET, ip, &sa->sin_addr) != 1) {
            return etl::unexpected(WSAGetLastError());
        }

        return sizeof(os_sockaddr_in_t);
    }
}
