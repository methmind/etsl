//
// Created by sexey on 27.07.2026.
//
module;
#include <winsock2.h>
#include <mswsock.h>
#include <cassert>

#include <etl/expected.h>
#include <etl/endianness.h>

module net.tcp_socket_driver.iocp;

import socket.types;

namespace etsl
{
    C_TCPSocketDriverIOCP::C_TCPSocketDriverIOCP(C_Reactor& reactor, const tcp_socket_driver_events_s& events) noexcept :
        reactor_(reactor), fd_(INVALID_SOCKET), state_(tcp_socket_state_e::NONE), pendingOps_(0),
        wasConnected_(false), cachedDisposeReason_(0), events_(events)
    {
        assert((events.onReadyRead.is_valid() &&
            events.onCommit.is_valid() &&
            events.onConnect.is_valid() &&
            events.onDisconnect.is_valid() &&
            events.onDisposed.is_valid()) &&
            "Invalid tcp_socket_driver_events_s struct!");

        this->readinessOperation_.callback = decltype(this->readinessOperation_.callback)::create<
            C_TCPSocketDriverIOCP, &C_TCPSocketDriverIOCP::onReadinessOperation>(*this);
        this->disposeOperation_.callback = decltype(this->disposeOperation_.callback)::create<
            C_TCPSocketDriverIOCP, &C_TCPSocketDriverIOCP::onDisposeOperation>(*this);
    }

    etl::expected<void, int32_t> C_TCPSocketDriverIOCP::connect(const C_Address& addr) noexcept
    {
        if (this->state_ != tcp_socket_state_e::NONE || this->pendingOps_) {
            return etl::unexpected(WSAEALREADY);
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

    etl::expected<void, int32_t> C_TCPSocketDriverIOCP::EphemeralBind(const socket_t fd) noexcept
    {
        sockaddr_in addrAny{};
        addrAny.sin_family = AF_INET;
        addrAny.sin_port = 0;
        addrAny.sin_addr.s_addr = etl::hton<uint32_t>(INADDR_ANY);

        if (bind(fd, reinterpret_cast<const sockaddr*>(&addrAny), sizeof(addrAny)) == SOCKET_ERROR) {
            return etl::unexpected(WSAGetLastError());
        }

        return {};
    }

    etl::expected<LPFN_CONNECTEX, int32_t> C_TCPSocketDriverIOCP::GetConnectEx(const socket_t fd) noexcept
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

    etl::expected<void, int32_t> C_TCPSocketDriverIOCP::ConnectEx(const socket_t fd, const C_Address& addr,
        OVERLAPPED& completion) noexcept
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

    etl::expected<void, int32_t> C_TCPSocketDriverIOCP::createConnectOperation(const C_Address& addr) noexcept
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
        this->state_ = tcp_socket_state_e::CONNECTING;

        return {};
    }

    etl::expected<void, int32_t> C_TCPSocketDriverIOCP::createReadProbeOperation() noexcept
    {
        if (this->state_ != tcp_socket_state_e::CONNECTED) {
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

    void C_TCPSocketDriverIOCP::beginTeardown(int32_t reason) noexcept
    {
        if (this->disposeOperation_.is_linked()) {
            return;
        }

        if (this->state_ != tcp_socket_state_e::DISPOSING) {
            this->wasConnected_ = (this->state_ == tcp_socket_state_e::CONNECTED);
            this->cachedDisposeReason_ = reason;
            this->state_ = tcp_socket_state_e::DISPOSING;
            this->fd_.dispose();
        }

        if (this->pendingOps_) {
            return;
        }

        this->state_ = tcp_socket_state_e::NONE;
        if (this->cachedDisposeReason_ == EXPLICIT_DISPOSE) {
            this->reactor_.detach(this->disposeOperation_);
            return;
        }

        (this->wasConnected_) ? this->events_.onDisconnect(reason) : this->events_.onConnect(reason);
    }

    void C_TCPSocketDriverIOCP::onConnectRoutine() noexcept
    {
        if (setsockopt(this->fd_.get(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == SOCKET_ERROR) {
            beginTeardown(WSAGetLastError());
            return;
        }

        this->state_ = tcp_socket_state_e::CONNECTED;
        this->events_.onConnect(0);

        if (const auto err = createReadProbeOperation(); !err) {
            beginTeardown(err.error());
        }
    }

    void C_TCPSocketDriverIOCP::onReadRoutine() noexcept
    {
        char peek = 0;
        const auto peekRes = recv(this->fd_.get(), &peek, sizeof(peek), MSG_PEEK);
        if (peekRes == 0) {
            beginTeardown(0);
            return;
        }

        if (peekRes > 0) {
            this->events_.onReadyRead(this->fd_.get());
        } else if (const auto err = WSAGetLastError(); err != WSAEWOULDBLOCK) {
            beginTeardown(err);
            return;
        }

        if (const auto err = createReadProbeOperation(); !err) {
            beginTeardown(err.error());
        }
    }

    void C_TCPSocketDriverIOCP::onReadinessOperation(uint32_t, const int32_t error) noexcept
    {
        this->pendingOps_--;
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
}
