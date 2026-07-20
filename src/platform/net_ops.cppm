//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/expected.h>

export module net.ops;

import net.types;

export namespace etsl
{
    etl::expected<size_t, int32_t> ParseIPV4(const char* ip, uint16_t port, os_sockaddr_storage_t& storage) noexcept;
}