//
// Created by sexey on 19.07.2026.
//
module;
#include <type_traits>
#include <winsock2.h>

#include <etl/delegate.h>
#include <etl/intrusive_list.h>

export module reactor.iocp:defs;

namespace etsl
{
    struct operation_iocp_s;

    using iocp_operation_callback_t = etl::delegate<void(operation_iocp_s& operation, uint32_t bytes, int32_t error)>;

    using iocp_simple_callback_t = etl::delegate<void()>;

    struct operation_iocp_s : WSAOVERLAPPED // NOLINT(*-pro-type-member-init)
    {
        iocp_operation_callback_t callback{};

        ~operation_iocp_s() = default;
        operation_iocp_s() = default;

        operation_iocp_s(const operation_iocp_s&) = delete;
        operation_iocp_s& operator=(const operation_iocp_s&) = delete;

        operation_iocp_s(operation_iocp_s&&) = delete;
        operation_iocp_s& operator=(operation_iocp_s&&) = delete;
    };

    struct task_iocp_s : WSAOVERLAPPED // NOLINT(*-pro-type-member-init)
    {
        iocp_simple_callback_t callback{};
    };

    struct dispose_operation_iocp_s : etl::bidirectional_link<0>
    {
        iocp_simple_callback_t callback{};
    };

    enum class iocp_code_e : uint8_t
    {
        SHUTDOWN = 0,
        TASK,
        IO,
        ADD_TIMER,
    };

    static_assert(std::is_base_of_v<WSAOVERLAPPED, operation_iocp_s>,
        "operation_iocp_s must inherit from WSAOVERLAPPED!");
}
