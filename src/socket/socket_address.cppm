//
// Created by sexey on 20.07.2026.
//
module;
#include <etl/expected.h>

export module socket.address;

import net.types;

export namespace etsl
{
    class C_Address
    {
    public:
        ~C_Address() noexcept = default;
        C_Address() noexcept = default;

        etl::expected<void, int32_t> initialize(const char* ip, uint16_t port) noexcept;

        [[nodiscard]] const os_sockaddr& data() const noexcept { return reinterpret_cast<const os_sockaddr&>(this->storage_); }

        [[nodiscard]] size_t size() const noexcept { return this->size_; }

    private:
        size_t size_{};
        os_sockaddr_storage_t storage_{};
    };
}