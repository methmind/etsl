#include <cstdint>
#include <winsock2.h>

import reactor;
import net.tcp_socket_driver;
import net.tcp_socket_driver.iocp;
import socket.address;
import socket.types;

etsl::C_Reactor reactor;

void on_connect(int32_t error);

void on_ready_read(etsl::socket_t fd);

void on_commit(etsl::C_Reactor::operation_t& operation, int32_t error);

void on_disconnect(int32_t error);

void on_disposed();

etsl::tcp_socket_driver_events_s events {
    .onReadyRead = on_ready_read,
    .onCommit = on_commit,
    .onConnect = on_connect,
    .onDisconnect = on_disconnect,
    .onDisposed = on_disposed,
};

etsl::C_TCPSocketDriver driver(reactor, events);

etsl::C_TCPSocketDriverIOCP::send_operation_t sendOperation;

const char* hello_world = "Hello, Server!\r\n";

void on_connect(int32_t error) {
    return;
}

void on_ready_read(etsl::socket_t fd) {
    char test[261]{};
    recv(fd, test, 260, 0);

    sendOperation.content = { (uint8_t*)hello_world, strlen(hello_world) };
    if (const auto err = driver.send(sendOperation); !err) {
        return;
    }

    return;
}

void on_commit(etsl::C_Reactor::operation_t& operation, int32_t error) {
    return;
}

void on_disconnect(int32_t error) {
    return;
}

void on_disposed() {
    return;
}

int main()
{
    if (const auto err = reactor.initialize(); !err) {
        return err.error();
    }

    etsl::C_Address address;
    if (const auto err = address.initialize("213.149.6.153", 3730); !err) {
        return err.error();
    }

    if (const auto err = driver.connect(address); !err) {
        return err.error();
    }

    reactor.run();
    return 0;
}
