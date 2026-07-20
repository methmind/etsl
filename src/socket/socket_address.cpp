//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/expected.h>

module socket.address;

import net.ops;

namespace etsl
{
    etl::expected<void, int32_t> C_Address::initialize(const char* ip, const uint16_t port) noexcept
    {
        auto result = ParseIPV4(ip, port, this->storage_);
        if (!result) {
            return etl::unexpected(result.error());
        }

        this->size_ = *result;
        return {};
    }
}
