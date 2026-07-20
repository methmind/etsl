//
// Created by sexey on 25.05.2026.
//
module;
#include <winsock2.h>
#include "etl/expected.h"

module win.wsa;

namespace etsl
{
    C_WSAInitializer::~C_WSAInitializer() noexcept
    {
        if (this->isInitialized_) {
            WSACleanup();
        }
    }

    etl::expected<void, int32_t> C_WSAInitializer::initialize() noexcept
    {
        if (this->isInitialized_) {
            return {};
        }

        if (const auto err = WSAStartup(MAKEWORD(2, 2), &this->wsaData_); err != NO_ERROR) {
            return etl::unexpected(err);
        }

        this->isInitialized_ = true;
        return {};
    }
}
