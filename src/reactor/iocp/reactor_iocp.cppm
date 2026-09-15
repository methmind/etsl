//
// Created by sexey on 23.05.2026.
//
module;
#include <winsock2.h>
#include <windows.h>
#include <etl/expected.h>
#include <etl/intrusive_list.h>

export module etsl.reactor:iocp;

import etsl.net;
import etsl.timer;

export import :iocp_defs;

export namespace etsl
{
    class C_ReactorIOCP
    {
    public:
        using operation_t = operation_iocp_s;
        using dispose_operation_t = dispose_operation_iocp_s;

        ~C_ReactorIOCP() noexcept;
        C_ReactorIOCP() noexcept : halt_(false), iocp_(nullptr) {}

        static int32_t TranslateError(const C_Socket& fd, const operation_t& operation, int32_t iocpError) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> initialize() noexcept;

        [[nodiscard]] etl::expected<void, int32_t> associate(socket_t fd) noexcept;

        template<typename op_t, typename post_t> requires std::derived_from<op_t, operation_t> && std::same_as<std::invoke_result_t<post_t&, op_t&>, etl::expected<void, int32_t>>
        [[nodiscard]] static etl::expected<void, int32_t> Assign(op_t& operation, post_t&& post) noexcept;

        void detach(dispose_operation_t& operation) noexcept;

        [[nodiscard]] etl::expected<void, int32_t> post(operation_t& task) noexcept;

        void schedule(C_Timer& timer, const timer_duration_t& duration) noexcept;

        void unschedule(C_Timer& timer) noexcept;

        void run() noexcept;

        void shutdown() noexcept;

    private:
        [[nodiscard]] uint32_t nextTimeout() const noexcept;

        [[nodiscard]] const dispose_operation_t* popDisposable() noexcept;

        bool halt_;
        HANDLE iocp_;

        C_TimerQueue timerQueue_;
        etl::intrusive_list<dispose_operation_t, etl::bidirectional_link<0>> disposable_;
    };

    template<typename op_t, typename post_t> requires std::derived_from<op_t, C_ReactorIOCP::operation_t> && std::same_as<std::invoke_result_t<post_t&, op_t&>, etl::expected<void, int32_t>>
    etl::expected<void, int32_t> C_ReactorIOCP::Assign(op_t& operation, post_t&& post) noexcept
    {
        if (operation.inFlight) {
            return etl::unexpected(WSAEALREADY);
        }

        memset(static_cast<WSAOVERLAPPED*>(&operation), 0, sizeof(WSAOVERLAPPED));
        if (const auto err = post(operation); !err) {
            return err;
        }

        operation.inFlight = true;
        return {};
    }
}
