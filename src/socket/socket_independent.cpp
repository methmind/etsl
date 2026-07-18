//
// Created by sexey on 29.05.2026.
//
module;

#if defined(_WIN32)
#include <winsock2.h>
#elif defined(__linux__)
#include <unistd.h>
#define closesocket close
#else
#error "Unsupported platform"
#endif

module socket.independent;

namespace etsl
{
    void CloseSocket(socket_t sock) noexcept
    {
        closesocket(sock);
    }
}
