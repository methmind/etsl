//
// Created by sexey on 24.08.2026.
//
module;
#include <winsock2.h>
#include <mswsock.h>

#include <etl/expected.h>
#include <etl/pool.h>

#include <util/noncopyable.h>

export module etsl.tcp.acceptor:iocp;

import etsl.net;
import etsl.reactor;

import :delegate;
import :defs_iocp;

export namespace etsl
{
    template<typename delegate_t>
    class C_TCPAcceptorIOCP
    {
    public:
        using accept_operation_t = accept_operation_s;

        ~C_TCPAcceptorIOCP() noexcept
        {
            assert(!this->disposeOperation_.is_linked() && "UAF error caught!");
            assert(this->backlog_.empty() && "UAF error caught!");
        }

        explicit C_TCPAcceptorIOCP(C_Reactor& reactor, etl::ipool& backlog, delegate_t& delegate) noexcept;

        ETSL_NON_COPYABLE_NON_MOVABLE(C_TCPAcceptorIOCP);

        void dispose() noexcept { beginTeardown(EXPLICIT_DISPOSE); }

        [[nodiscard]] etl::expected<void, int32_t> initialize(const C_Address& addr) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> listen(int32_t backlog) noexcept;

    private:
        [[nodiscard]] static etl::expected<C_Address, int32_t> GetRemotePeerAddr(socket_t fd, void* acceptBuffer) noexcept;

        [[nodiscard]] static etl::expected<void, int32_t> AcceptEx(socket_t gateway,
            accept_operation_t& operation) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> rearmAcceptOperation(accept_operation_t& operation) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> armAcceptBacklog() noexcept;

        void beginTeardown(int32_t reason) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> acceptIncoming(accept_operation_t& operation, int32_t lastError) noexcept;

        void onIncoming(C_Reactor::operation_t& operation, uint32_t /*transferred*/, int32_t error) noexcept;

        void onDisposeOperation() noexcept;

        C_Reactor& reactor_;
        C_Reactor::dispose_operation_t disposeOperation_{};

        C_Socket gateway_;
        tcp_acceptor_state_e state_;

        int32_t cachedDisposeReason_;

        delegate_t& delegate_;
        etl::ipool& backlog_;
    };

    template<typename delegate_t>
    C_TCPAcceptorIOCP<delegate_t>::C_TCPAcceptorIOCP(C_Reactor& reactor, etl::ipool& backlog, delegate_t& delegate) noexcept :
        reactor_(reactor), gateway_(INVALID_SOCKET), state_(tcp_acceptor_state_e::NONE),
        cachedDisposeReason_(INVALID_CACHE_VALUE), delegate_(delegate), backlog_(backlog)
    {
        static_assert(TCPAcceptorDelegate<delegate_t>, "Delegate must satisfy tcp acceptor delegate trait!");

        this->disposeOperation_.callback = decltype(this->disposeOperation_.callback)::template create<
            C_TCPAcceptorIOCP, &C_TCPAcceptorIOCP::onDisposeOperation>(*this);
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPAcceptorIOCP<delegate_t>::initialize(const C_Address& addr) noexcept
    {
        if (this->state_ != tcp_acceptor_state_e::NONE || this->gateway_.is_valid()) {
            return etl::unexpected(WSAEALREADY);
        }

        if (!this->backlog_.capacity()) {
            return etl::unexpected(WSAENOBUFS);
        }

        if (this->backlog_.max_item_size() != sizeof(accept_operation_t) || !this->backlog_.empty()) {
            return etl::unexpected(WSAEINVAL);
        }

        auto gate = CreateSocket();
        if (!gate) {
            return etl::unexpected(gate.error());
        }

        int32_t exclusiveAddrUse = 1;
        if (setsockopt(gate->get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusiveAddrUse), sizeof(exclusiveAddrUse)) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        if (bind(gate->get(), &addr.data(), static_cast<int32_t>(addr.size())) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        if (const auto err = this->reactor_.associate(gate->get()); !err) {
            return etl::unexpected(err.error());
        }

        this->gateway_ = etl::move(*gate);
        return {};
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPAcceptorIOCP<delegate_t>::listen(int32_t backlog) noexcept
    {
        if (this->state_ != tcp_acceptor_state_e::NONE || !this->gateway_.is_valid()) {
            return etl::unexpected(WSAEINVAL);
        }

        auto fallback = [this](int32_t err) -> etl::expected<void, int32_t> {
            this->state_ = tcp_acceptor_state_e::NONE;
            this->gateway_.dispose();

            return etl::unexpected(err);
        };

        if (::listen(this->gateway_.get(), backlog) == SOCKET_ERROR) {
            return fallback(WSAGetLastError());
        }

        this->state_ = tcp_acceptor_state_e::LISTENING;
        if (const auto err = armAcceptBacklog(); !err) {
            return fallback(err.error());
        }

        return {};
    }

    template<typename delegate_t>
    etl::expected<C_Address, int32_t> C_TCPAcceptorIOCP<delegate_t>::GetRemotePeerAddr(socket_t fd, void* acceptBuffer) noexcept {
        auto getResult = GetExtensionFunction<WSAID_GETACCEPTEXSOCKADDRS>(fd);
        if (!getResult) {
            return etl::unexpected(getResult.error());
        }

        os_sockaddr* local{};
        os_sockaddr* remote{};
        uint32_t localSize, remoteSize;
        const auto getAcceptExSockaddrs = reinterpret_cast<LPFN_GETACCEPTEXSOCKADDRS>(*getResult);
        getAcceptExSockaddrs(acceptBuffer, 0, ACCEPT_BUFFER_SIZE,
            ACCEPT_BUFFER_SIZE, &local, reinterpret_cast<LPINT>(&localSize),
            &remote, reinterpret_cast<LPINT>(&remoteSize)
        );

        if (!remote) {
            return etl::unexpected(WSAEINVAL);
        }

        C_Address addr;
        if (!addr.initialize(*remote, remoteSize)) {
            return etl::unexpected(WSAEINVAL);
        }

        return addr;
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPAcceptorIOCP<delegate_t>::AcceptEx(socket_t gateway, accept_operation_t& operation) noexcept
    {
        auto getResult = GetExtensionFunction<WSAID_ACCEPTEX>(gateway);
        if (!getResult) {
            return etl::unexpected(getResult.error());
        }

        DWORD tmp = 0;
        const auto acceptEx = reinterpret_cast<LPFN_ACCEPTEX>(*getResult);
        if (!acceptEx(gateway, operation.fd.get(), static_cast<void*>(&operation.buffer), 0,
            ACCEPT_BUFFER_SIZE, ACCEPT_BUFFER_SIZE, &tmp, static_cast<WSAOVERLAPPED*>(&operation))) {
            if (const auto err = WSAGetLastError(); err != WSA_IO_PENDING) {
                return etl::unexpected(err);
            }
        }

        return {};
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPAcceptorIOCP<delegate_t>::rearmAcceptOperation(accept_operation_t& operation) noexcept
    {
        if (this->state_ != tcp_acceptor_state_e::LISTENING) {
            return etl::unexpected(static_cast<int32_t>(WSA_OPERATION_ABORTED));
        }

        auto newDescriptor = CreateSocket();
        if (!newDescriptor) {
            return etl::unexpected(newDescriptor.error());
        }

        C_Reactor::FlushOperation(operation);
        operation.fd = etl::move(*newDescriptor);
        operation.callback = decltype(accept_operation_t::callback)::create<
           C_TCPAcceptorIOCP, &C_TCPAcceptorIOCP::onIncoming>(*this);

        if (const auto err = AcceptEx(this->gateway_.get(), operation); !err) {
            return err;
        }

        return {};
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPAcceptorIOCP<delegate_t>::armAcceptBacklog() noexcept
    {
        if (this->state_ != tcp_acceptor_state_e::LISTENING) {
            return etl::unexpected(static_cast<int32_t>(WSA_OPERATION_ABORTED));
        }

        etl::expected<void, int32_t> lastError;
        auto destructor = [backlog = &backlog_](auto ptr) -> void {
            backlog->destroy<accept_operation_t>(ptr);
        };

        for (auto i = this->backlog_.size(); i < this->backlog_.capacity(); ++i) {
            auto operation = etl::unique_ptr<accept_operation_t, decltype(destructor)>(this->backlog_.create<accept_operation_t>(), destructor);
            if (!operation) {
                continue;
            }

            if (lastError = rearmAcceptOperation(*operation); !lastError) {
                continue;
            }

            operation.release();
        }

        if (this->backlog_.empty()) {
            return lastError;
        }

        return {};
    }

    template<typename delegate_t>
    void C_TCPAcceptorIOCP<delegate_t>::beginTeardown(int32_t reason) noexcept
    {
        if (this->cachedDisposeReason_ == INVALID_CACHE_VALUE || reason == EXPLICIT_DISPOSE) {
            this->cachedDisposeReason_ = reason;
        }

        if (this->gateway_.is_valid()) {
            this->gateway_.dispose();
        }

        this->state_ = tcp_acceptor_state_e::DISPOSING;
        if (!this->backlog_.empty()) {
            return;
        }

        if (this->disposeOperation_.is_linked()) {
            return;
        }

        this->reactor_.detach(this->disposeOperation_);
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPAcceptorIOCP<delegate_t>::acceptIncoming(accept_operation_t& operation, int32_t lastError) noexcept
    {
        if (this->state_ != tcp_acceptor_state_e::LISTENING) {
            return etl::unexpected(static_cast<int32_t>(WSA_OPERATION_ABORTED));
        }

        if (lastError != ERROR_SUCCESS) {
            return etl::unexpected(C_Reactor::TranslateError(this->gateway_, operation, lastError));
        }

        socket_t listening = this->gateway_.get();
        if (setsockopt(operation.fd.get(), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<char*>(&listening), sizeof(listening)) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        auto addrResult = GetRemotePeerAddr(listening, operation.buffer);
        if (!addrResult) {
            return etl::unexpected(addrResult.error());
        }

        this->delegate_.onIncoming(etl::move(operation.fd), *addrResult);
        return {};
    }

    template<typename delegate_t>
    void C_TCPAcceptorIOCP<delegate_t>::onIncoming(C_Reactor::operation_t& operation,
        uint32_t /*transferred*/, int32_t error) noexcept
    {
        auto& acceptOperation = static_cast<accept_operation_t&>(operation);
        if (const auto err = acceptIncoming(acceptOperation, error); !err && err.error() != WSA_OPERATION_ABORTED) {
            this->delegate_.onError(err.error());
        }

        if (const auto err = rearmAcceptOperation(acceptOperation); !err) {
            this->backlog_.destroy<accept_operation_t>(&acceptOperation);
            (err.error() != WSA_OPERATION_ABORTED) ? this->delegate_.onError(err.error()) : void();
        }

        if (const auto err = armAcceptBacklog(); !err) {
            beginTeardown(err.error());
        }
    }

    template<typename delegate_t>
    void C_TCPAcceptorIOCP<delegate_t>::onDisposeOperation() noexcept
    {
        assert(this->backlog_.empty() && "Dangling operations!");

        const auto stackReason = this->cachedDisposeReason_;
        this->state_ = tcp_acceptor_state_e::NONE;
        this->cachedDisposeReason_ = INVALID_CACHE_VALUE;

        this->delegate_.onDisposed(stackReason);
    }
}
