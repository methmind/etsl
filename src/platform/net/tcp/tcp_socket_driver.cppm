//
// Created by sexey on 27.07.2026.
//
export module net.tcp_socket_driver;

export import :defs;

#if defined(_WIN32)
import :iocp;
export namespace etsl
{
    template<typename T>
    using C_TCPSocketDriver = C_TCPSocketDriverIOCP<T>;
}
#elif defined(__linux__)
#error "Linux tcp socket driver is not implemented yet"
#else
#error "Unsupported platform"
#endif
