#include <mswsock.h>
#include <etl/expected.h>
#include <etl/endianness.h>
#include <etl/delegate.h>
#include <winsock2.h>
#include <cassert>

import reactor;
import socket.types;
import socket.raii;
import socket.address;
import socket.factory;

enum class socket_state_e : uint8_t
{
    NONE,
    CONNECTING,
    CONNECTED,
    DISPOSING,
};

using on_ready_read_callback_t = etl::delegate<void(etsl::socket_t fd)>;

using on_commit_callback_t = etl::delegate<void(etsl::C_Reactor::operation_t& operation, int32_t error)>;

using on_connect_callback_t = etl::delegate<void(int32_t error)>;

using on_disconnect_callback_t = etl::delegate<void(int32_t error)>;

using on_disposed_callback_t = etl::delegate<void()>;

struct tcp_driver_events_s
{
    on_ready_read_callback_t onReadyRead;
    on_commit_callback_t onCommit;
    on_connect_callback_t onConnect;
    on_disconnect_callback_t onDisconnect;
    on_disposed_callback_t onDisposed;
};

constexpr int32_t EXPLICIT_DISPOSE = -1;

class C_TCPSocketDriverIOCP
{
public:
    ~C_TCPSocketDriverIOCP() noexcept
    {
        assert(this->pendingOps_ == 0 && "UAF error caught!");
    }

    explicit C_TCPSocketDriverIOCP(etsl::C_Reactor& reactor, const tcp_driver_events_s& events) noexcept :
        reactor_(reactor), fd_(INVALID_SOCKET), state_(socket_state_e::NONE), pendingOps_(0), wasConnected_(false),
        disposeReason_(0),  events_(events)
    {
        assert((events.onReadyRead.is_valid() &&
            events.onCommit.is_valid() &&
            events.onConnect.is_valid() &&
            events.onDisconnect.is_valid() &&
            events.onDisposed.is_valid()) &&
            "Invalid tcp_driver_events_s struct!");

        this->readinessOperation_.callback = decltype(this->readinessOperation_.callback)::create<
            C_TCPSocketDriverIOCP, &C_TCPSocketDriverIOCP::onReadinessOperation>(*this);
        this->disposeOperation_.callback = decltype(this->disposeOperation_.callback)::create<
            C_TCPSocketDriverIOCP, &C_TCPSocketDriverIOCP::onDisposeOperation>(*this);
    }

    void dispose() noexcept
    {
        beginTeardown(EXPLICIT_DISPOSE);
    }

    [[nodiscard]] etl::expected<void, int32_t> connect(const etsl::C_Address& addr) noexcept
    {
        if (this->state_ != socket_state_e::NONE || this->pendingOps_) {
            return etl::unexpected(WSAEALREADY);
        }

        auto socketCreateResult = etsl::CreateSocket();
        if (!socketCreateResult) {
            return etl::unexpected(socketCreateResult.error());
        }

        this->fd_ = std::move(*socketCreateResult);
        if (const auto err = createConnectOperation(addr); !err) {
            this->fd_.dispose();
            return etl::unexpected(err.error());
        }

        return {};
    }

private:
    [[nodiscard]] static etl::expected<void, int32_t> EphemeralBind(const etsl::socket_t fd) noexcept
    {
        constexpr sockaddr_in addrAny = {
            .sin_family = AF_INET,
            .sin_port = 0,
            .sin_addr.s_addr = etl::hton<uint32_t>(INADDR_ANY),
            .sin_zero = {}
        };

        if (bind(fd, reinterpret_cast<const sockaddr*>(&addrAny), sizeof(addrAny)) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        return {};
    }

    [[nodiscard]] static etl::expected<LPFN_CONNECTEX, int32_t> GetConnectEx(const etsl::socket_t fd) noexcept
    {
        DWORD bytes = 0;
        GUID fnGUID = WSAID_CONNECTEX;
        LPFN_CONNECTEX fnPtr = nullptr;
        if (WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &fnGUID, sizeof(fnGUID), &fnPtr, sizeof(fnPtr), &bytes, nullptr, nullptr) != ERROR_SUCCESS) {
            return etl::unexpected(WSAGetLastError());
        }

        return fnPtr;
    }

    [[nodiscard]] static etl::expected<void, int32_t> ConnectEx(const etsl::socket_t fd, const etsl::C_Address& addr, OVERLAPPED& completion) noexcept
    {
        static auto getResult{GetConnectEx(fd)};
        if (!getResult) {
            return etl::unexpected(getResult.error());
        }

        const auto connectEx = *getResult;
        if (!connectEx(fd, &addr.data(), static_cast<int>(addr.size()), nullptr, 0, nullptr, &completion)) {
            if (const auto err = WSAGetLastError(); err != WSA_IO_PENDING) {
                return etl::unexpected(err);
            }
        }

        return {};
    }

    void flushReadinessOperation() noexcept
    {
        memset(&this->readinessOperation_, 0, sizeof(WSAOVERLAPPED));
    }

    [[nodiscard]] etl::expected<void, int32_t> createConnectOperation(const etsl::C_Address& addr) noexcept
    {
        if (const auto err = this->reactor_.associate(this->fd_.get()); !err) {
            return etl::unexpected(err.error());
        }

        if (const auto err = EphemeralBind(this->fd_.get()); !err) {
            return etl::unexpected(err.error());
        }

        flushReadinessOperation();
        if (const auto err = ConnectEx(this->fd_.get(), addr, this->readinessOperation_); !err) {
            return etl::unexpected(err.error());
        }

        this->pendingOps_++;
        this->state_ = socket_state_e::CONNECTING;

        return {};
    }

    [[nodiscard]] etl::expected<void, int32_t> createReadProbeOperation() noexcept
    {
        if (this->state_ != socket_state_e::CONNECTED) {
            return etl::unexpected(WSAEINVAL);
        }

        DWORD flags = 0;
        WSABUF tmp{};

        flushReadinessOperation();
        if (WSARecv(this->fd_.get(), &tmp, 1, nullptr, &flags, &this->readinessOperation_, nullptr) == SOCKET_ERROR) {
            if (const auto err = WSAGetLastError(); err != WSA_IO_PENDING) {
                return etl::unexpected(err);
            }
        }

        this->pendingOps_++;
        return {};
    }

    void beginTeardown(int32_t reason) noexcept
    {
        if (this->state_ != socket_state_e::DISPOSING) {
            this->wasConnected_ = (this->state_ == socket_state_e::CONNECTED);
            this->disposeReason_ = reason;
            this->state_ = socket_state_e::DISPOSING;
            this->fd_.dispose();
        }

        if (this->pendingOps_) { // Не все I/O операции завершены
            return;
        }

        this->state_ = socket_state_e::NONE;
        if (this->disposeReason_ == EXPLICIT_DISPOSE) { // Если EXPLICIT_DISPOSE, то сетевого уведомления не шлём
            this->reactor_.detach(this->disposeOperation_);
            return;
        }

        (this->wasConnected_) ? this->events_.onDisconnect(reason) : this->events_.onConnect(reason);
    }

    void onConnectRoutine() noexcept
    {
        if (setsockopt(this->fd_.get(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == SOCKET_ERROR) {
            beginTeardown(WSAGetLastError());
            return;
        }

        this->state_ = socket_state_e::CONNECTED;
        this->events_.onConnect(0);

        if (const auto err = createReadProbeOperation(); !err) {
            beginTeardown(err.error());
        }
    }

    void onReadRoutine() noexcept
    {
        char peek = 0;
        const auto peekRes = recv(this->fd_.get(), &peek, 1, MSG_PEEK);
        if (peekRes == 0) { // FIN TCP
            beginTeardown(0);
            return;
        }

        if (peekRes == SOCKET_ERROR) {
            if (const auto err = WSAGetLastError(); err != WSAEWOULDBLOCK) {
                beginTeardown(err);
                return;
            }
        }

        this->events_.onReadyRead(this->fd_.get());
        if (const auto err = createReadProbeOperation(); !err) {
            beginTeardown(err.error());
        }
    }

    void onReadinessOperation(uint32_t /* transferred */, int32_t error) noexcept
    {
        this->pendingOps_--;
        if (error != ERROR_SUCCESS) {
            beginTeardown(error);
            return;
        }

        switch (this->state_) {
            case socket_state_e::CONNECTING:
                onConnectRoutine();
                break;
            case socket_state_e::CONNECTED:
                onReadRoutine();
                break;
            default:
                assert(false && "Invalid state!");
                break;
        }
    }

    void onDisposeOperation() const noexcept
    {
        // Последний вызов драйвера.
        // Пользователь может уничтожить объект внутри callback.
        // После вызова callback никаких обращений к полям класса не допускается.
        this->events_.onDisposed();
    }

    etsl::C_Reactor& reactor_;
    etsl::C_Reactor::operation_t readinessOperation_{};
    etsl::C_Reactor::dispose_operation_t disposeOperation_{};

    etsl::C_Socket fd_{};
    socket_state_e state_;
    uint32_t pendingOps_;

    bool wasConnected_;
    int32_t disposeReason_;

    tcp_driver_events_s events_;
};

void on_ready_read(etsl::socket_t fd)
{
    char test[128]{};

    auto res = recv(fd, test, 128, 0);
    if (res == 0) {
        return;
    }

    return;
}

void on_commit(etsl::C_Reactor::operation_t& operation, int32_t error)
{
    return;
}

void on_connect(int32_t error)
{
    return;
}

void on_disconnect(int32_t error)
{
    return;
}

void on_disposed()
{
    return;
}

int main()
{
    etsl::C_Reactor reactor;
    if (const auto err = reactor.initialize(); !err) {
        return err.error();
    }

    etsl::C_Address addr;
    if (const auto err = addr.initialize("129.6.15.28", 13); !err) {
        return err.error();
    }

    tcp_driver_events_s events {
        .onReadyRead = on_ready_read,
        .onCommit = on_commit,
        .onConnect = on_connect,
        .onDisconnect = on_disconnect,
        .onDisposed = on_disposed,
    };

    C_TCPSocketDriverIOCP tcpDriver(reactor, events);
    if (const auto err = tcpDriver.connect(addr); !err) {
        return err.error();
    }

    reactor.run();
    return 0;
}
