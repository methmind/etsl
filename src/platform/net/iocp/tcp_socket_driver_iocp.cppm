//
// Created by sexey on 27.07.2026.
//
module;
#include <winsock2.h>
#include <mswsock.h>

#include <etl/span.h>
#include <etl/expected.h>
#include <etl/endianness.h>

export module net.tcp_socket_driver:iocp;

import reactor;
import socket.types;
import socket.raii;
import socket.address;
import socket.factory;
import :defs;
import :delegate;
import :defs_iocp;

#define FlushOperation(operation) ZeroMemory(&(operation), sizeof(WSAOVERLAPPED))

export namespace etsl
{
    template<typename delegate_t>
    class C_TCPSocketDriverIOCP
    {
    public:
        ~C_TCPSocketDriverIOCP() noexcept { assert(this->pendingOps_ == 0 && "UAF error caught!"); }

        explicit C_TCPSocketDriverIOCP(C_Reactor& reactor, delegate_t& delegate) noexcept;

        void dispose() noexcept { beginTeardown(EXPLICIT_DISPOSE); }

        [[nodiscard]] etl::expected<void, int32_t> connect(const C_Address& addr) noexcept;

        [[nodiscard]] etl::expected<uint32_t, int32_t> read(const etl::span<uint8_t>& content) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> send(send_operation_t& operation) noexcept
        {
            return createSendOperation(operation);
        }

    private:
        [[nodiscard]] static etl::expected<void, int32_t> EphemeralBind(socket_t fd) noexcept;

        [[nodiscard]] static etl::expected<LPFN_CONNECTEX, int32_t> GetConnectEx(socket_t fd) noexcept;

        [[nodiscard]] static etl::expected<void, int32_t> ConnectEx(socket_t fd, const C_Address& addr,
            WSAOVERLAPPED& completion) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> createConnectOperation(const C_Address& addr) noexcept;

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
        tcp_socket_state_e state_;
        uint32_t pendingOps_;

        bool wasConnected_;
        int32_t cachedDisposeReason_;

        delegate_t& delegate_;
    };

    template<typename delegate_t>
    C_TCPSocketDriverIOCP<delegate_t>::C_TCPSocketDriverIOCP(C_Reactor& reactor, delegate_t& delegate) noexcept :
        reactor_(reactor), fd_(INVALID_SOCKET_VALUE), state_(tcp_socket_state_e::NONE), pendingOps_(0),
        wasConnected_(false), cachedDisposeReason_(INVALID_CACHE_VALUE), delegate_(delegate)
    {
        static_assert(TCPDriverDelegate<delegate_t>, "Delegate must satisfy tcp driver delegate trait!");

        this->readinessOperation_.callback = decltype(this->readinessOperation_.callback)::template create<
            C_TCPSocketDriverIOCP, &C_TCPSocketDriverIOCP::onReadinessOperation>(*this);

        this->disposeOperation_.callback = decltype(this->disposeOperation_.callback)::template create<
            C_TCPSocketDriverIOCP, &C_TCPSocketDriverIOCP::onDisposeOperation>(*this);
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPSocketDriverIOCP<delegate_t>::connect(const C_Address& addr) noexcept
    {
        if (this->state_ != tcp_socket_state_e::NONE || this->pendingOps_) {
            return etl::unexpected(static_cast<int32_t>(WSAEALREADY));
        }

        auto socketCreateResult = CreateSocket();
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

    template<typename delegate_t>
    etl::expected<uint32_t, int32_t> C_TCPSocketDriverIOCP<delegate_t>::read(const etl::span<uint8_t>& content) noexcept
    {
        if (this->state_ != tcp_socket_state_e::CONNECTED) {
            return etl::unexpected(WSAENOTCONN);
        }

        auto fail = [this](int32_t err) -> etl::expected<uint32_t, int32_t> {
            beginTeardown(err);
            return etl::unexpected(err);
        };

        const auto res = recv(this->fd_.get(), reinterpret_cast<char*>(content.data()), static_cast<int32_t>(content.size()), 0);
        if (res > 0) {
            return res;
        }

        if (res == 0) {
            return fail(0);
        }

        if (const auto err = WSAGetLastError(); err != WSAEWOULDBLOCK) {
            return fail(err);
        }

        return 0;
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPSocketDriverIOCP<delegate_t>::EphemeralBind(socket_t fd) noexcept
    {
        constexpr sockaddr_in addrAny{
            .sin_family = AF_INET,
            .sin_port = 0,
            .sin_addr.s_addr = etl::hton<uint32_t>(INADDR_ANY)
        };

        if (bind(fd, reinterpret_cast<const sockaddr*>(&addrAny), sizeof(addrAny)) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        return {};
    }

    template<typename delegate_t>
    etl::expected<LPFN_CONNECTEX, int32_t> C_TCPSocketDriverIOCP<delegate_t>::GetConnectEx(socket_t fd) noexcept
    {
        DWORD bytes = 0;
        GUID fnGUID = WSAID_CONNECTEX;
        LPFN_CONNECTEX fnPtr = nullptr;
        if (WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &fnGUID, sizeof(fnGUID), &fnPtr, sizeof(fnPtr),
            &bytes, nullptr, nullptr) != ERROR_SUCCESS) {
            return etl::unexpected(WSAGetLastError());
        }

        return fnPtr;
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPSocketDriverIOCP<delegate_t>::ConnectEx(socket_t fd, const C_Address& addr,
        WSAOVERLAPPED& completion) noexcept
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

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPSocketDriverIOCP<delegate_t>::createConnectOperation(const C_Address& addr) noexcept
    {
        if (const auto err = this->reactor_.associate(this->fd_.get()); !err) {
            return etl::unexpected(err.error());
        }

        if (const auto err = EphemeralBind(this->fd_.get()); !err) {
            return etl::unexpected(err.error());
        }

        FlushOperation(this->readinessOperation_);
        if (const auto err = ConnectEx(this->fd_.get(), addr, this->readinessOperation_); !err) {
            return etl::unexpected(err.error());
        }

        ++this->pendingOps_;
        this->state_ = tcp_socket_state_e::CONNECTING;

        return {};
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPSocketDriverIOCP<delegate_t>::createReadProbeOperation() noexcept
    {
        if (this->state_ != tcp_socket_state_e::CONNECTED) {
            return etl::unexpected(static_cast<int32_t>(WSAEINVAL));
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
    etl::expected<void, int32_t> C_TCPSocketDriverIOCP<delegate_t>::createSendOperation(send_operation_t& operation) noexcept
    {
        if (this->state_ != tcp_socket_state_e::CONNECTED) {
            return etl::unexpected(static_cast<int32_t>(WSAEINVAL));
        }

        FlushOperation(operation);
        operation.callback = decltype(operation.callback)::create<
            C_TCPSocketDriverIOCP, &C_TCPSocketDriverIOCP::onSendOperation>(*this);

        WSABUF buffer = {
            .len = static_cast<uint32_t>(operation.content.size() - operation.transferred),
            .buf = reinterpret_cast<char*>(operation.content.data() + operation.transferred),
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
    void C_TCPSocketDriverIOCP<delegate_t>::beginTeardown(int32_t reason) noexcept
    {
        if (this->cachedDisposeReason_ == INVALID_CACHE_VALUE || reason == EXPLICIT_DISPOSE) {
            this->cachedDisposeReason_ = reason;
        }

        if (this->fd_.is_valid()) {
            this->wasConnected_ = (this->state_ == tcp_socket_state_e::CONNECTED);
            this->fd_.dispose();
        }

        this->state_ = tcp_socket_state_e::DISPOSING;
        if (this->pendingOps_) {
            return;
        }

        if (this->disposeOperation_.is_linked()) {
            return;
        }

        this->reactor_.detach(this->disposeOperation_);
    }

    template<typename delegate_t>
    void C_TCPSocketDriverIOCP<delegate_t>::onConnectRoutine() noexcept
    {
        if (setsockopt(this->fd_.get(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != ERROR_SUCCESS) {
            beginTeardown(WSAGetLastError());
            return;
        }

        this->state_ = tcp_socket_state_e::CONNECTED;
        this->delegate_.onConnect(0);

        if (const auto err = createReadProbeOperation(); !err) {
            beginTeardown(err.error());
        }
    }

    template<typename delegate_t>
    void C_TCPSocketDriverIOCP<delegate_t>::onReadRoutine() noexcept
    {
        this->delegate_.onReadyRead();
        if (this->state_ != tcp_socket_state_e::CONNECTED) {
            return;
        }

        if (const auto err = createReadProbeOperation(); !err) {
            beginTeardown(err.error());
        }
    }

    template<typename delegate_t>
    void C_TCPSocketDriverIOCP<delegate_t>::onReadinessOperation(C_Reactor::operation_t& /*operation*/,
        uint32_t /*transferred*/, int32_t error) noexcept
    {
        --this->pendingOps_;
        if (error != ERROR_SUCCESS) {
            beginTeardown(error);
            return;
        }

        switch (this->state_) {
            case tcp_socket_state_e::CONNECTING:
                onConnectRoutine();
                break;
            case tcp_socket_state_e::CONNECTED:
                onReadRoutine();
                break;
            case tcp_socket_state_e::DISPOSING:
                beginTeardown(this->cachedDisposeReason_);
                break;
            default:
                assert(false && "Invalid state!");
                break;
        }
    }

    template<typename delegate_t>
    void C_TCPSocketDriverIOCP<delegate_t>::onSendOperation(C_Reactor::operation_t& operation, uint32_t transferred,
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

        if (this->state_ == tcp_socket_state_e::DISPOSING) {
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
    void C_TCPSocketDriverIOCP<delegate_t>::onDisposeOperation() noexcept
    {
        assert(this->pendingOps_ == 0 && "Dangling operations!");

        auto stackReason = this->cachedDisposeReason_;
        this->state_ = tcp_socket_state_e::NONE;
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
