//
// Created by sexey on 31.07.2026.
//
module;
#include <etl/span.h>

export module etsl.tcp.connection:defs;

import etsl.reactor;

export namespace etsl
{
    struct send_operation_s : C_Reactor::operation_t
    {
        uint32_t transferred;
        etl::span<const uint8_t> content;
    };

    using send_operation_t = send_operation_s;
}
