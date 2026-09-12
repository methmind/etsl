//
// Created by sexey on 27.07.2026.
//
export module etsl.tcp.connection;

#if defined(_WIN32)
export import :iocp;
export namespace etsl
{
    template<typename T>
    using C_TCPConnection = C_TCPConnectionIOCP<T>;
}
#elif defined(__linux__)
#error "Linux tcp socket driver is not implemented yet"
#else
#error "Unsupported platform"
#endif
