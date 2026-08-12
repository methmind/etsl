//
// Created by sexey on 19.07.2026.
//
module;
#include <type_traits>
#include <winsock2.h>

#include <etl/delegate.h>
#include <etl/intrusive_list.h>

#include <util/noncopyable.h>

export module reactor:iocp_defs;

namespace etsl
{
    struct operation_iocp_s : WSAOVERLAPPED // NOLINT(*-pro-type-member-init)
    {
        etl::delegate<void(operation_iocp_s& operation, uint32_t bytes, int32_t error)> callback{};

        ETSL_DEFAULT_NON_COPYABLE_NON_MOVABLE(operation_iocp_s)
    };

    using task_iocp_s = operation_iocp_s;

    struct dispose_operation_iocp_s : etl::bidirectional_link<0>
    {
        etl::delegate<void()> callback{};
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
