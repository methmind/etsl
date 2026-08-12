#include <cstdint>
#include <winsock2.h>

import reactor;
import net;
import net.tcp_socket_driver;

const char* hello_world = "Hello, Server!\r\n";

class C_Test
{
public:
    ~C_Test() noexcept = default;

    explicit C_Test(etsl::C_Reactor& reactor) noexcept :
        reactor_(reactor), driver_(reactor, *this) {}

    void onConnect(int32_t error) noexcept
    {
        return;
    }

    void onReadyRead() noexcept
    {
        char test[261]{};
        if (const auto err = this->driver_.read({(uint8_t*)&test, sizeof(test)}); !err) {
            return;
        }

        if (const auto err = this->driver_.send({(uint8_t*)hello_world, strlen(hello_world)},
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

    auto exec(const etsl::C_Address& addr) noexcept
    {
        return this->driver_.connect(addr);
    }

private:
    etsl::C_Reactor& reactor_;
    etsl::C_TCPSocketDriver<C_Test> driver_;

    etsl::send_operation_t sendOperation_{};
};

int main()
{
    etsl::C_Reactor reactor;
    if (const auto err = reactor.initialize(); !err) {
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
