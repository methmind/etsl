//
// Created by sexey on 28.08.2026.
//
module;
#include <winsock2.h>

#include <etl/expected.h>

module etsl.init;

namespace etsl
{
    class C_WSAInitializer
    {
    public:
        ~C_WSAInitializer() noexcept
        {
            if (this->isInitialized_) {
                WSACleanup();
            }
        }

        C_WSAInitializer() noexcept : isInitialized_(false), wsaData_({}) {}

        [[nodiscard]] etl::expected<void, int32_t> initialize() noexcept
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
    private:
        bool isInitialized_;
        WSADATA wsaData_;
    };

    bool Initialize() noexcept
    {
        static C_WSAInitializer wsa;
        if (const auto err = wsa.initialize(); !err) {
            return false;
        }

        return true;
    }
}
