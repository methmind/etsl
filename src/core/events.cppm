//
// Created by sexey on 18.07.2026.
//
module;
#include "etl/delegate.h"

export module socket.events;

export namespace etsl
{
    enum class socket_event_flags_e : uint8_t
    {
        EV_READ   = 0x1,
        EV_WRITE  = 0x2,
        EV_CLOSED = 0x4,
        EV_ERROR  = 0x8,
    };

    using socket_event_callback_t = etl::delegate<void(socket_event_flags_e event, int32_t error)>;
}
