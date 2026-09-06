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

#define FlushOperation(operation) memset(static_cast<WSAOVERLAPPED*>(&(operation)), 0, sizeof(WSAOVERLAPPED))

export namespace etsl
{
    template<typename delegate_t>
    class C_TCPAcceptorIOCP
    {
    public:
        using accept_operation_t = accept_operation_s;

        ~C_TCPAcceptorIOCP() noexcept { assert(this->backlog_.empty() && "UAF error caught!"); }

        explicit C_TCPAcceptorIOCP(C_Reactor& reactor, etl::ipool& backlog, delegate_t& delegate) noexcept;

        ETSL_NON_COPYABLE_NON_MOVABLE(C_TCPAcceptorIOCP);

        void dispose() noexcept { beginTeardown(EXPLICIT_DISPOSE); }

        [[nodiscard]] etl::expected<void, int32_t> initialize(const C_Address& addr) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> listen(int32_t backlog) noexcept;

    private:
        [[nodiscard]] static etl::expected<void, int32_t> AcceptEx(socket_t gateway,
            accept_operation_t& operation) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> rearmAcceptOperation(accept_operation_t& operation) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> armAcceptBacklog() noexcept;

        void beginTeardown(int32_t reason) noexcept;

        void onConnectionIncoming(C_Reactor::operation_t& operation, uint32_t /*transferred*/, int32_t error) noexcept;

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
        if (!this->backlog_.capacity()) {
            return etl::unexpected(static_cast<int32_t>(ERROR_NOT_ENOUGH_MEMORY));
        }

        if (this->backlog_.max_item_size() != sizeof(accept_operation_t)) {
            return etl::unexpected(static_cast<int32_t>(MEM_E_INVALID_SIZE));
        }

        if (this->state_ != tcp_acceptor_state_e::NONE) {
            return etl::unexpected(WSAEALREADY);
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

        this->gateway_ = std::move(*gate);
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

            /* @note
             * Ни одна AcceptEx не взведена - акцептор не начал работу. Отдаём код ошибки
             * синхронно; терминального колбэка не будет.
            */
            if (this->backlog_.empty()) {
                return etl::unexpected(err);
            }

            beginTeardown(err);
            return {};
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
    etl::expected<void, int32_t> C_TCPAcceptorIOCP<delegate_t>::AcceptEx(socket_t gateway,
        accept_operation_t& operation) noexcept
    {
        static auto getResult{GetExtensionFunction(gateway, WSAID_ACCEPTEX)};
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
            return etl::unexpected(WSAEINVAL);
        }

        auto newDescriptor = CreateSocket();
        if (!newDescriptor) {
            return etl::unexpected(newDescriptor.error());
        }

        FlushOperation(operation);
        operation.fd = std::move(*newDescriptor);
        operation.callback = decltype(accept_operation_t::callback)::create<
           C_TCPAcceptorIOCP, &C_TCPAcceptorIOCP::onConnectionIncoming>(*this);

        if (const auto err = AcceptEx(this->gateway_.get(), operation); !err) {
            return etl::unexpected(err.error());
        }

        return {};
    }

    template<typename delegate_t>
    etl::expected<void, int32_t> C_TCPAcceptorIOCP<delegate_t>::armAcceptBacklog() noexcept
    {
        while (this->backlog_.size() < this->backlog_.capacity()) {
            auto destructor = [backlog = &backlog_](auto ptr) -> void {
                backlog->destroy<accept_operation_t>(ptr);
            };

            auto operation = etl::unique_ptr<accept_operation_t, decltype(destructor)>(this->backlog_.create<accept_operation_t>(), destructor);
            /*if (!operation) { Impossible???
                return etl::unexpected(static_cast<int32_t>(ERROR_NOT_ENOUGH_MEMORY));
            }*/

            if (const auto err = rearmAcceptOperation(*operation); !err) {
                return etl::unexpected(err.error());
            }

            operation.release();
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
    void C_TCPAcceptorIOCP<delegate_t>::onConnectionIncoming(C_Reactor::operation_t& operation,
        uint32_t /*transferred*/, int32_t error) noexcept
    {
        auto& acceptOperation = reinterpret_cast<accept_operation_t&>(operation);
        auto fallback = [this](int32_t err, const accept_operation_t& op) -> void {
            this->backlog_.destroy<accept_operation_t>(&op);
            beginTeardown(err);
        };

        if (this->state_ == tcp_acceptor_state_e::DISPOSING) {
            fallback(ERROR_OPERATION_ABORTED, acceptOperation);
            return;
        }

        if (error == ERROR_SUCCESS) {
            this->delegate_.onIncoming(std::move(acceptOperation.fd));
        }

        if (const auto err = rearmAcceptOperation(acceptOperation); !err) {
            fallback(err.error(), acceptOperation);
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
