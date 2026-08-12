//
// Created by sexey on 25.05.2026.
//
module;
#include <winsock2.h>
#include "etl/expected.h"

export module win.wsa;

export namespace etsl
{
    class C_WSAInitializer
    {
    public:
        ~C_WSAInitializer() noexcept;
        C_WSAInitializer() noexcept : isInitialized_(false), wsaData_({}) {}

        [[nodiscard]] etl::expected<void, int32_t> initialize() noexcept;
    private:
        bool isInitialized_;
        WSADATA wsaData_;
    };
}
