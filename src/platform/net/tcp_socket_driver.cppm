//
// Created by sexey on 27.07.2026.
//
module;

export module net.tcp_socket_driver;

export import net.tcp_socket_driver.types;

#if defined(_WIN32)
import net.tcp_socket_driver.iocp;
export namespace etsl { using C_TCPSocketDriver = C_TCPSocketDriverIOCP; }
#elif defined(__linux__)
#error "Linux tcp socket driver is not implemented yet"
#else
#error "Unsupported platform"
#endif
