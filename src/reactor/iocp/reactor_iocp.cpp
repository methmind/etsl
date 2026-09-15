//
// Created by sexey on 23.05.2026.
//
module;
#include <winsock2.h>
#include <windows.h>

#include <etl/chrono.h>
#include "etl/expected.h"

module etsl.reactor;

import :iocp;

namespace etsl
{
    C_ReactorIOCP::~C_ReactorIOCP() noexcept
    {
        if (this->iocp_ != nullptr) {
            CloseHandle(this->iocp_);
        }
    }

    int32_t C_ReactorIOCP::TranslateError(const C_Socket& fd, const operation_t& operation, int32_t iocpError) noexcept
    {
        if (iocpError == ERROR_SUCCESS) {
            return iocpError;
        }

        DWORD flags = 0, transferred = 0;
        if (WSAGetOverlappedResult(fd.get(), const_cast<LPOVERLAPPED>(static_cast<const WSAOVERLAPPED*>(&operation)), &transferred, false, &flags)) {
            return iocpError; // Расхождение с реактором, но остаёмся пессимистами.
        }

        return WSAGetLastError();
    }

    etl::expected<void, int32_t> C_ReactorIOCP::initialize() noexcept
    {
        if (this->iocp_ != nullptr) {
            return {};
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

    etl::expected<void, int32_t> C_ReactorIOCP::post(operation_t& task) noexcept
    {
        if (!PostQueuedCompletionStatus(this->iocp_, 0, static_cast<ULONG_PTR>(iocp_code_e::TASK), &task)) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    void C_ReactorIOCP::schedule(C_Timer& timer, const timer_duration_t& duration) noexcept
    {
        this->timerQueue_.schedule(timer, duration);
    }

    void C_ReactorIOCP::unschedule(C_Timer& timer) noexcept
    {
        this->timerQueue_.unschedule(timer);
    }

    void C_ReactorIOCP::run() noexcept
    {
        while (this->halt_ == false) {
            DWORD bytesTransferred = 0;
            ULONG_PTR completionKey = 0;
            WSAOVERLAPPED* overlapped = nullptr;

            const auto ioStatus = GetQueuedCompletionStatus(this->iocp_, &bytesTransferred, &completionKey, &overlapped, nextTimeout());
            const auto ioError = (ioStatus) ? 0 : static_cast<int32_t>(GetLastError());

            if (overlapped) {
                const auto operation = reinterpret_cast<operation_t*>(overlapped);
                operation->inFlight = false;
                operation->callback(*operation, bytesTransferred, ioError);
            }

            const auto currentTime = steady_clock_t::now();
            while (const auto timer = this->timerQueue_.pop(currentTime)) {
                timer->execute();
            }

            while (const auto disposable = popDisposable()) {
                disposable->callback();
            }
        }
    }

    void C_ReactorIOCP::shutdown() noexcept
    {
        this->halt_ = true;
        PostQueuedCompletionStatus(this->iocp_, 0, static_cast<ULONG_PTR>(iocp_code_e::SHUTDOWN), nullptr);
    }

    uint32_t C_ReactorIOCP::nextTimeout() const noexcept
    {
        if (!this->disposable_.empty()) {
            return 0;
        }

        const auto nearestTimepoint = this->timerQueue_.nearest();
        if (!nearestTimepoint) {
            return INFINITE;
        }

        const auto currentTime = steady_clock_t::now();
        if (*nearestTimepoint <= currentTime) {
            return 0;
        }

        const auto timeout = etl::ceil<timer_duration_t>(*nearestTimepoint - currentTime).count();
        if (timeout >= INFINITE) {
            assert(false && "Timeout is too long!");
            return INFINITE;
        }

        return static_cast<uint32_t>(timeout);
    }

    const C_ReactorIOCP::dispose_operation_t* C_ReactorIOCP::popDisposable() noexcept
    {
        if (this->disposable_.empty()) {
            return nullptr;
        }

        const auto ptr = &this->disposable_.front();
        this->disposable_.pop_front();

        return ptr;
    }
}
