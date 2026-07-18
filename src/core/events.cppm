//
// Created by sexey on 18.07.2026.
//
module;
#include <cstdint>

#include "etl/delegate.h"

export module core.events;

export namespace etsl
{
    enum EventFlags : uint8_t
    {
        EV_READ   = 0x1,
        EV_WRITE  = 0x2,
        EV_CLOSED = 0x4,
        EV_ERROR  = 0x8,
    };

    using C_EventCallback = etl::delegate<void(uint8_t events, int32_t error)>;
}
