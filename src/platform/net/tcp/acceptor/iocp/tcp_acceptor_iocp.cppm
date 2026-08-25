//
// Created by sexey on 24.08.2026.
//
module;
#include <winsock2.h>
#include <mswsock.h>

#include <etl/expected.h>
#include <etl/pool.h>

#include <util/noncopyable.h>

export module net.tcp_acceptor:iocp;

import net;
import socket;
import reactor;

import :defs;

export namespace etsl
{
    class C_TCPAcceptorIOCP
    {
    public:
        ~C_TCPAcceptorIOCP() noexcept = default;

        explicit C_TCPAcceptorIOCP(C_Reactor& reactor) noexcept : reactor_(reactor), gateway_(INVALID_SOCKET) {}

        ETSL_NON_COPYABLE_NON_MOVABLE(C_TCPAcceptorIOCP);

        etl::expected<void, int32_t> initialize(const C_Address& addr) noexcept
        {
            auto gate = CreateSocket();
            if (!gate) {
                return etl::unexpected(gate.error());
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

        etl::expected<void, int32_t> listen(int32_t backlog) noexcept
        {
            if (::listen(this->gateway_.get(), backlog) == SOCKET_ERROR) {
                return etl::unexpected(WSAGetLastError());
            }

            for (auto i = 0; i < this->pool_.capacity(); ++i) {
                if (const auto err = postAccept(); !err) {
                    return etl::unexpected(err.error());
                }
            }

            return {};
        }

    private:
        static etl::expected<void, int32_t> AcceptEx(socket_t gateway, const accept_operation_t& operation) noexcept
        {
            static auto getResult{GetExtensionFunction(gateway, WSAID_ACCEPTEX)};
            if (!getResult) {
                return etl::unexpected(getResult.error());
            }

            DWORD tmp = 0;
            const auto acceptEx = static_cast<LPFN_ACCEPTEX>(*getResult);
            if (!acceptEx(gateway, operation.fd.get(), (void*)&operation.buffer, 0,
                ACCEPT_BUFFER_SIZE,
                ACCEPT_BUFFER_SIZE, &tmp, (WSAOVERLAPPED*)&operation)) {
                if (const auto err = WSAGetLastError(); err != WSA_IO_PENDING) {
                    return etl::unexpected(err);
                }
            }

            return {};
        }

        etl::expected<void, int32_t> postAccept() noexcept
        {
            const auto operation = this->pool_.create();
            if (!operation) {
                return etl::unexpected(static_cast<int32_t>(ERROR_NOT_ENOUGH_MEMORY));
            }

            auto newFD = CreateSocket();
            if (!newFD) {
                return etl::unexpected(newFD.error());
            }

            operation->fd = std::move(*newFD);
            operation->callback = decltype(accept_operation_t::callback)::create<
                C_TCPAcceptorIOCP, &C_TCPAcceptorIOCP::onClientAccepted>(*this);

            if (const auto err = AcceptEx(this->gateway_.get(), *operation); !err) {
                return etl::unexpected(err.error());
            }

            return {};
        }

        void onClientAccepted(C_Reactor::operation_t& operation, uint32_t /*transferred*/, int32_t error) noexcept
        {
            auto& casted = reinterpret_cast<accept_operation_t&>(operation);
            auto newFD = std::move(casted.fd);
            this->pool_.destroy(&casted);

            if (error != ERROR_SUCCESS) {
                //todo begin teardown...
                return;
            }

            if (const auto err = postAccept(); !err && err.error() != ERROR_NOT_ENOUGH_MEMORY) {
                //todo begin teardown...
                return;
            }
        }

        C_Reactor& reactor_;
        C_Socket gateway_;

        etl::pool<accept_operation_t, 32> pool_;
    };
}