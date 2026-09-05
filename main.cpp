#include <cstdint>
#include <winsock2.h>
#include <etl/pool.h>
#include <util/noncopyable.h>

#include "etl/expected.h"

import etsl;

const char* hello_world = "Hello, Server!\r\n";

class C_Test : etsl::C_TCPConnection<C_Test>
{
public:
    friend etsl::C_TCPConnection<C_Test>;

    ~C_Test() noexcept = default;

    explicit C_Test(etsl::C_Reactor& reactor) noexcept : etsl::C_TCPConnection<C_Test>(reactor, *this), reactor_(reactor) {}

    auto exec(const etsl::C_Address& addr) noexcept
    {
        return connect(addr);
    }

private:
    void onConnect(int32_t error) noexcept
    {
        return;
    }

    void onReadyRead() noexcept
    {
        char test[261]{};
        if (const auto err = read({(uint8_t*)&test, sizeof(test)}); !err) {
            return;
        }

        if (const auto err = send({(uint8_t*)hello_world, strlen(hello_world)},
            this->sendOperation_); !err) {
            return;
        }

        return;
    }

    void onCommit(etsl::C_Reactor::operation_t& operation, int32_t error) noexcept
    {
        return;
    }

    void onDisconnect(int32_t error) noexcept
    {
        return;
    }

    void onDisposed() noexcept
    {
        return;
    }

    etsl::C_Reactor& reactor_;
    etsl::send_operation_t sendOperation_{};
};

class C_Server : public etsl::C_TCPAcceptor<C_Server>
{
public:
    friend etsl::C_TCPAcceptor<C_Server>;

    ~C_Server() noexcept = default;

    explicit C_Server(etsl::C_Reactor& reactor) noexcept :
        etsl::C_TCPAcceptor<C_Server>(reactor, backlog, *this) {}

private:
    void onIncoming(etsl::C_Socket fd) noexcept
    {
        return;
    }

    void onDisposed() noexcept
    {
        return;
    }

    etl::pool<accept_operation_t, 1> backlog{};
};

int main()
{
    if (!etsl::Initialize()) {
        return -1;
    }

    etsl::C_Reactor reactor;
    if (const auto err = reactor.initialize(); !err) {
        return err.error();
    }

    etsl::C_Address gateAddr;
    if (const auto err = gateAddr.initialize("0.0.0.0", 3730); !err) {
        return err.error();
    }

    C_Server server(reactor);
    if (const auto err = server.initialize(gateAddr); !err) {
        return err.error();
    }

    if (const auto err = server.listen(SOMAXCONN); !err) {
        return err.error();
    }

    etsl::C_Address address;
    if (const auto err = address.initialize("127.0.0.1", 3730); !err) {
        return err.error();
    }

    C_Test test(reactor);
    if (const auto err = test.exec(address); !err) {
        return err.error();
    }

    reactor.run();
    return 0;
}
