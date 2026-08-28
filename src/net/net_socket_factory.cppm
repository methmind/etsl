//
// Created by sexey on 29.05.2026.
//
module;
#include <etl/expected.h>

export module etsl.net:factory;

import :socket;

export namespace etsl
{
    etl::expected<C_Socket, int32_t> CreateSocket() noexcept;
}
