//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/expected.h>

export module net:address;

import :defs;

export namespace etsl
{
    class C_Address
    {
    public:
        ~C_Address() noexcept = default;
        C_Address() noexcept = default;

        etl::expected<void, int32_t> initialize(const char* ip, uint16_t port) noexcept;

        [[nodiscard]] const os_sockaddr& data() const noexcept { return reinterpret_cast<const os_sockaddr&>(this->storage_); }

        [[nodiscard]] uint32_t size() const noexcept { return this->size_; }

    private:
        uint32_t size_{};
        os_sockaddr_storage_t storage_{};
    };
}