//
// Created by sexey on 27.07.2026.
//
module;
#include <winsock2.h>
#include <mswsock.h>

#include <etl/span.h>
#include <etl/expected.h>

export module net.tcp_socket_driver.iocp;

import reactor;
import socket.types;
import socket.raii;
import socket.address;
import socket.factory;
import net.tcp_socket_driver.types;
import :defs;

export namespace etsl
{
    class C_TCPSocketDriverIOCP
    {
    public:
        using send_operation_t = send_operation_s;

        ~C_TCPSocketDriverIOCP() noexcept;

        explicit C_TCPSocketDriverIOCP(C_Reactor& reactor, const tcp_socket_driver_events_s& events) noexcept;

        void dispose() noexcept;

        [[nodiscard]] etl::expected<void, int32_t> connect(const C_Address& addr) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> send(send_operation_t& operation) noexcept;

    private:
        [[nodiscard]] static etl::expected<void, int32_t> EphemeralBind(socket_t fd) noexcept;

        [[nodiscard]] static etl::expected<LPFN_CONNECTEX, int32_t> GetConnectEx(socket_t fd) noexcept;

        [[nodiscard]] static etl::expected<void, int32_t> ConnectEx(socket_t fd, const C_Address& addr,
            OVERLAPPED& completion) noexcept;

        void flushReadinessOperation() noexcept;

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

        C_Socket fd_{};
        tcp_socket_state_e state_;
        uint32_t pendingOps_;

        bool wasConnected_;
        int32_t cachedDisposeReason_;

        tcp_socket_driver_events_s events_;
    };
}
