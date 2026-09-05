//
// Created by sexey on 27.07.2026.
//
module;
#include <winsock2.h>
#include <mswsock.h>

#include <etl/span.h>
#include <etl/expected.h>
#include <etl/endianness.h>

#include <util/noncopyable.h>

export module etsl.tcp.connection:iocp;

import etsl.reactor;
import etsl.net;

import :defs;
import :delegate;
import :defs_iocp;

#define FlushOperation(operation) memset(static_cast<WSAOVERLAPPED*>(&(operation)), 0, sizeof(WSAOVERLAPPED))

export namespace etsl
{
    template<typename delegate_t>
    class C_TCPConnectionIOCP
    {
    public:
        ~C_TCPConnectionIOCP() noexcept { assert(this->pendingOps_ == 0 && "UAF error caught!"); }

        explicit C_TCPConnectionIOCP(C_Reactor& reactor, delegate_t& delegate) noexcept;

        ETSL_NON_COPYABLE_NON_MOVABLE(C_TCPConnectionIOCP);

        void dispose() noexcept { beginTeardown(EXPLICIT_DISPOSE); }

        [[nodiscard]] etl::expected<void, int32_t> connect(const C_Address& addr) noexcept;

        [[nodiscard]] etl::expected<uint32_t, int32_t> read(const etl::span<uint8_t>& content) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> send(const etl::span<const uint8_t>& content, send_operation_t& operation) noexcept
        {
            operation.content = content;
            return createSendOperation(operation);
        }

    private:
        [[nodiscard]] static etl::expected<void, int32_t> EphemeralBind(socket_t fd) noexcept;

        [[nodiscard]] static etl::expected<void, int32_t> ConnectEx(socket_t fd, const C_Address& addr,
            WSAOVERLAPPED& completion) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> createConnectOperation(const C_Address& addr, C_Socket&& fd) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> createReadProbeOperation() noexcept;

        [[nodiscard]] etl::expected<void, int32_t> createSendOperation(send_operation_t& operation) noexcept;

        void beginTeardown(int32_t reason) noexcept;

        void onConnectRoutine() noexcept;

        void onReadRoutine() noexcept;

        void onReadinessOperation(C_Reactor::operation_t& operation, uint32_t transferred, int32_t error) noexcept;

        void onSendOperation(C_Reactor::operation_t& operation, uint32_t transferred, int32_t error) noexcept;

        void onDisposeOperation() noexcept;

        C_Reactor& reactor_;
        C_Reactor::operation_t readinessOperation_{};
        C_Reactor::dispose_operation_t disposeOperation_{};

        C_Socket fd_;
        tcp_connection_state_e state_;
        uint16_t pendingOps_;

        bool wasConnected_;
        int32_t cachedDisposeReason_;

        delegate_t& delegate_;
    };

    template<typename delegate_t>
    C_TCPConnectionIOCP<delegate_t>::C_TCPConnectionIOCP(C_Reactor& reactor, delegate_t& delegate) noexcept :
        reactor_(reactor), fd_(INVALID_SOCKET), state_(tcp_connection_state_e::NONE), pendingOps_(0),
        wasConnected_(false), cachedDisposeReason_(INVALID_CACHE_VALUE), delegate_(delegate)
    {
        static_assert(TCPConnectionDelegate<delegate_t>, "Delegate must satisfy tcp connection delegate trait!");

        this->readinessOperation_.callback = decltype(C_Reactor::operation_t::callback)::create<
            C_TCPConnectionIOCP, &C_TCPConnectionIOCP::onReadinessOperation>(*this);

        this->disposeOperation_.callback = decltype(C_Reactor::dispose_operation_t::callback)::create<
            C_TCPConnectionIOCP, &C_TCPConnectionIOCP::onDisposeOperation>(*this);
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPConnectionIOCP<delegate_t>::connect(const C_Address& addr) noexcept
    {
        if (this->state_ != tcp_connection_state_e::NONE || this->pendingOps_) {
            return etl::unexpected(WSAEALREADY);
        }

        auto socketCreateResult = CreateSocket();
        if (!socketCreateResult) {
            return etl::unexpected(socketCreateResult.error());
        }

        if (const auto err = createConnectOperation(addr, std::move(*socketCreateResult)); !err) {
            return etl::unexpected(err.error());
        }

        return {};
    }

    template<typename delegate_t>
    etl::expected<uint32_t, int32_t> C_TCPConnectionIOCP<delegate_t>::read(const etl::span<uint8_t>& content) noexcept
    {
        if (content.empty()) {
            return etl::unexpected(WSAEINVAL);
        }

        if (this->state_ != tcp_connection_state_e::CONNECTED) {
            return etl::unexpected(WSAENOTCONN);
        }

        auto fallback = [this](int32_t err) -> etl::expected<uint32_t, int32_t> {
            beginTeardown(err);
            return etl::unexpected(err);
        };

        const auto res = recv(this->fd_.get(), reinterpret_cast<char*>(content.data()), static_cast<int32_t>(content.size()), 0);
        if (res > 0) {
            return res;
        }

        if (res == 0) {
            return fallback(0);
        }

        if (const auto err = WSAGetLastError(); err != WSAEWOULDBLOCK) {
            return fallback(err);
        }

        return 0;
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPConnectionIOCP<delegate_t>::EphemeralBind(socket_t fd) noexcept
    {
        constexpr os_sockaddr_in_t addrAny{
            .sin_family = AF_INET,
            .sin_port = 0,
            .sin_addr.s_addr = etl::hton<uint32_t>(INADDR_ANY)
        };

        if (bind(fd, reinterpret_cast<const os_sockaddr*>(&addrAny), sizeof(addrAny)) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        return {};
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPConnectionIOCP<delegate_t>::ConnectEx(socket_t fd, const C_Address& addr,
        WSAOVERLAPPED& completion) noexcept
    {
        static auto getResult{GetExtensionFunction(fd, WSAID_CONNECTEX)};
        if (!getResult) {
            return etl::unexpected(getResult.error());
        }

        const auto connectEx = reinterpret_cast<LPFN_CONNECTEX>(*getResult);
        if (!connectEx(fd, &addr.data(), static_cast<int32_t>(addr.size()), nullptr, 0, nullptr, &completion)) {
            if (const auto err = WSAGetLastError(); err != WSA_IO_PENDING) {
                return etl::unexpected(err);
            }
        }

        return {};
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPConnectionIOCP<delegate_t>::createConnectOperation(const C_Address& addr, C_Socket&& fd) noexcept
    {
        if (const auto err = this->reactor_.associate(fd.get()); !err) {
            return etl::unexpected(err.error());
        }

        if (const auto err = EphemeralBind(fd.get()); !err) {
            return etl::unexpected(err.error());
        }

        FlushOperation(this->readinessOperation_);
        if (const auto err = ConnectEx(fd.get(), addr, this->readinessOperation_); !err) {
            return etl::unexpected(err.error());
        }

        ++this->pendingOps_;
        this->state_ = tcp_connection_state_e::CONNECTING;
        this->fd_ = std::move(fd);

        return {};
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPConnectionIOCP<delegate_t>::createReadProbeOperation() noexcept
    {
        if (this->state_ != tcp_connection_state_e::CONNECTED) {
            return etl::unexpected(WSAEINVAL);
        }

        DWORD flags = 0;
        WSABUF tmp{};

        FlushOperation(this->readinessOperation_);
        if (WSARecv(this->fd_.get(), &tmp, 1, nullptr, &flags, &this->readinessOperation_, nullptr) == SOCKET_ERROR) {
            if (const auto err = WSAGetLastError(); err != WSA_IO_PENDING) {
                return etl::unexpected(err);
            }
        }

        ++this->pendingOps_;
        return {};
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPConnectionIOCP<delegate_t>::createSendOperation(send_operation_t& operation) noexcept
    {
        if (this->state_ != tcp_connection_state_e::CONNECTED) {
            return etl::unexpected(WSAEINVAL);
        }

        FlushOperation(operation);
        operation.callback = decltype(operation.callback)::create<
            C_TCPConnectionIOCP, &C_TCPConnectionIOCP::onSendOperation>(*this);

        WSABUF buffer = {
            .len = static_cast<uint32_t>(operation.content.size() - operation.transferred),
            .buf = reinterpret_cast<char*>(const_cast<uint8_t*>(operation.content.data() + operation.transferred)),
        };

        if (WSASend(this->fd_.get(), &buffer, 1, nullptr, 0, &operation, nullptr) != ERROR_SUCCESS) {
            if (const auto err = WSAGetLastError(); err != WSA_IO_PENDING) {
                return etl::unexpected(err);
            }
        }

        ++this->pendingOps_;
        return {};
    }

    template<typename delegate_t>
    void C_TCPConnectionIOCP<delegate_t>::beginTeardown(int32_t reason) noexcept
    {
        if (this->cachedDisposeReason_ == INVALID_CACHE_VALUE || reason == EXPLICIT_DISPOSE) {
            this->cachedDisposeReason_ = reason;
        }

        if (this->fd_.is_valid()) {
            this->wasConnected_ = (this->state_ == tcp_connection_state_e::CONNECTED);
            this->fd_.dispose();
        }

        this->state_ = tcp_connection_state_e::DISPOSING;
        if (this->pendingOps_) {
            return;
        }

        if (this->disposeOperation_.is_linked()) {
            return;
        }

        this->reactor_.detach(this->disposeOperation_);
    }

    template<typename delegate_t>
    void C_TCPConnectionIOCP<delegate_t>::onConnectRoutine() noexcept
    {
        if (setsockopt(this->fd_.get(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == SOCKET_ERROR) {
            beginTeardown(WSAGetLastError());
            return;
        }

        this->state_ = tcp_connection_state_e::CONNECTED;
        this->delegate_.onConnect(0);

        if (const auto err = createReadProbeOperation(); !err) {
            beginTeardown(err.error());
        }
    }

    template<typename delegate_t>
    void C_TCPConnectionIOCP<delegate_t>::onReadRoutine() noexcept
    {
        this->delegate_.onReadyRead();
        if (const auto err = createReadProbeOperation(); !err) {
            beginTeardown(err.error());
        }
    }

    template<typename delegate_t>
    void C_TCPConnectionIOCP<delegate_t>::onReadinessOperation(C_Reactor::operation_t& /*operation*/,
        uint32_t /*transferred*/, int32_t error) noexcept
    {
        --this->pendingOps_;
        if (error != ERROR_SUCCESS) {
            beginTeardown(error);
            return;
        }

        switch (this->state_) {
            case tcp_connection_state_e::CONNECTING:
                onConnectRoutine();
                break;
            case tcp_connection_state_e::CONNECTED:
                onReadRoutine();
                break;
            case tcp_connection_state_e::DISPOSING:
                beginTeardown(0); // Тут код ошибки не важен (ибо уже кешировано), но указать что-то надо.
                break;
            default:
                assert(false && "Invalid state!");
                break;
        }
    }

    template<typename delegate_t>
    void C_TCPConnectionIOCP<delegate_t>::onSendOperation(C_Reactor::operation_t& operation, uint32_t transferred,
        int32_t error) noexcept
    {
        auto finalize = [this, &operation](int32_t error) noexcept {
            if (error != ERROR_SUCCESS) {
                beginTeardown(error);
            }

            this->delegate_.onCommit(operation, error);
        };

        --this->pendingOps_;
        if (error != ERROR_SUCCESS) {
            finalize(error);
            return;
        }

        if (this->state_ == tcp_connection_state_e::DISPOSING) {
            finalize(ERROR_OPERATION_ABORTED);
            return;
        }

        auto& sendOperation = reinterpret_cast<send_operation_t&>(operation);
        sendOperation.transferred += transferred;
        if (sendOperation.transferred < sendOperation.content.size()) {
            if (const auto err = createSendOperation(sendOperation); !err) {
                finalize(err.error());
            }

            return;
        }

        finalize(ERROR_SUCCESS);
    }

    template<typename delegate_t>
    void C_TCPConnectionIOCP<delegate_t>::onDisposeOperation() noexcept
    {
        assert(this->pendingOps_ == 0 && "Dangling operations!");

        auto stackReason = this->cachedDisposeReason_;
        this->state_ = tcp_connection_state_e::NONE;
        this->cachedDisposeReason_ = INVALID_CACHE_VALUE;

        if (stackReason == EXPLICIT_DISPOSE) {
            this->delegate_.onDisposed();
            return;
        }

        (this->wasConnected_) ?
            this->delegate_.onDisconnect(stackReason) :
            this->delegate_.onConnect(stackReason);
    }
}
