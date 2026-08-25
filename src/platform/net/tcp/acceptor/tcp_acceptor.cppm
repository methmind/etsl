//
// Created by sexey on 24.08.2026.
//

export module net.tcp_acceptor;

export import :defs;

#if defined(_WIN32)
import :iocp;
export namespace etsl
{
    using C_TCPAcceptor = C_TCPAcceptorIOCP;
}
#elif defined(__linux__)
#error "Linux tcp socket driver is not implemented yet"
#else
#error "Unsupported platform"
#endif
