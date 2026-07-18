//
// Created by sexey on 23.05.2026.
//
module;
#include <winsock2.h>
#include <windows.h>
#include <cstdint>

#include "etl/expected.h"

module event_loop.iocp;

import win.wsa;

namespace etsl
{
    C_EventLoopIOCP::~C_EventLoopIOCP() noexcept
    {
        if (this->iocp_ != nullptr) {
            CloseHandle(this->iocp_);
        }
    }

    etl::expected<void, int32_t> C_EventLoopIOCP::initialize() noexcept
    {
        static C_WSAInitializer wsa;
        if (const auto res = wsa.initialize(); !res) {
            return etl::unexpected(res.error());
        }

        this->iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (this->iocp_ == nullptr) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    void C_EventLoopIOCP::run() noexcept
    {
        while (this->halt_.load() == false) {
            DWORD bytesTransferred = 0;
            ULONG_PTR completionKey = 0;
            WSAOVERLAPPED* overlapped = nullptr;

            auto ioStatus = GetQueuedCompletionStatus(this->iocp_, &bytesTransferred, &completionKey,
                &overlapped, INFINITE
            );

            if (completionKey == 0 && overlapped == nullptr) { // Timeout or force awake
                continue;
            }

            if (!ioStatus) {
                continue;
            }
        }
    }

    void C_EventLoopIOCP::shutdown() noexcept
    {
        this->halt_.store(true);
        PostQueuedCompletionStatus(this->iocp_, 0, 0, nullptr);
    }
}
