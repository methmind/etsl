//
// Created by sexey on 23.05.2026.
//
module;
#include <winsock2.h>
#include <windows.h>

#include "etl/expected.h"

module reactor;

import :iocp;
import win.wsa;

namespace etsl
{
    C_ReactorIOCP::~C_ReactorIOCP() noexcept
    {
        if (this->iocp_ != nullptr) {
            CloseHandle(this->iocp_);
        }
    }

    etl::expected<void, int32_t> C_ReactorIOCP::initialize() noexcept
    {
        if (this->iocp_ != nullptr) {
            return {};
        }

        static C_WSAInitializer wsa;
        if (const auto res = wsa.initialize(); !res) {
            return etl::unexpected(res.error());
        }

        this->iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (!this->iocp_) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    etl::expected<void, int32_t> C_ReactorIOCP::associate(const socket_t fd) noexcept
    {
        if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(fd), this->iocp_,
            static_cast<ULONG_PTR>(iocp_code_e::IO), 0)) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    void C_ReactorIOCP::detach(dispose_operation_t& operation) noexcept
    {
        this->disposable_.push_back(operation);
    }

    etl::expected<void, int32_t> C_ReactorIOCP::post(task_t& task) noexcept
    {
        if (!PostQueuedCompletionStatus(this->iocp_, 0, static_cast<ULONG_PTR>(iocp_code_e::TASK), &task)) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    void C_ReactorIOCP::addTimer(C_Timer& timer) noexcept
    {
        this->timerBucket_.add(timer);
    }

    void C_ReactorIOCP::removeTimer(C_Timer& timer) noexcept
    {
        this->timerBucket_.remove(timer);
    }

    void C_ReactorIOCP::run() noexcept
    {
        while (this->halt_ == false) {
            DWORD bytesTransferred = 0;
            ULONG_PTR completionKey = 0;
            WSAOVERLAPPED* overlapped = nullptr;

            const auto timeout = (this->disposable_.empty()) ? this->timerBucket_.nextTimeout(clock_t::now()) : 0;
            const auto ioStatus = GetQueuedCompletionStatus(this->iocp_, &bytesTransferred, &completionKey, &overlapped, timeout);

            const auto currentTime = clock_t::now();
            while (const auto timer = this->timerBucket_.pop(currentTime)) {
                timer->execute();
            }

            if (overlapped) {
                const auto operation = reinterpret_cast<operation_t*>(overlapped);
                operation->callback(*operation, bytesTransferred, ioStatus ? 0 : static_cast<int32_t>(GetLastError()));
            }

            while (!this->disposable_.empty()) {
                this->disposable_.back().callback();
                this->disposable_.pop_back();
            }
        }
    }

    void C_ReactorIOCP::shutdown() noexcept
    {
        this->halt_ = true;
        PostQueuedCompletionStatus(this->iocp_, 0, static_cast<ULONG_PTR>(iocp_code_e::SHUTDOWN), nullptr);
    }
}
