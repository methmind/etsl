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
export module net.types;

export namespace etsl
{
    using os_sockaddr_storage_t = sockaddr_storage;

    using os_sockaddr_in_t = sockaddr_in;

    using os_sockaddr = sockaddr;
}