//
// Created by sexey on 23.05.2026.
//
module;
#include <winsock2.h>
#include <windows.h>
#include <ntdef.h>

#include <etl/chrono.h>
#include <etl/expected.h>
#include <etl/span.h>

module etsl.reactor;

import :iocp;

namespace etsl
{
    C_ReactorIOCP::~C_ReactorIOCP() noexcept
    {
        if (this->iocp_) {
            CloseHandle(this->iocp_);
        }
    }

    etl::expected<void, int32_t> C_ReactorIOCP::initialize() noexcept
    {
        if (this->iocp_) {
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
        if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(fd), this->iocp_, fd, 0)) {
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
        if (const auto err = task.assign([this](WSAOVERLAPPED* operation) noexcept -> etl::expected<void, int32_t> {
            if (!PostQueuedCompletionStatus(this->iocp_, 0, 0, operation)) {
                return etl::unexpected(static_cast<int32_t>(GetLastError()));
            }

            return {};
        }); !err) {
            return err;
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

    etl::expected<void, int32_t> C_ReactorIOCP::run() noexcept
    {
        etl::expected<void, int32_t> err;
        while (!this->halt_.load(std::memory_order_acquire) && err) {
            err = ProceedOperations(this->iocp_, nextTimeout());

            const auto currentTime = steady_clock_t::now();
            while (const auto timer = this->timerQueue_.pop(currentTime)) {
                timer->execute();
            }

            while (const auto disposable = popDisposable()) {
                disposable->delegate();
            }
        }

        return err;
    }

    etl::expected<void, int32_t> C_ReactorIOCP::shutdown() noexcept
    {
        this->halt_.store(true, std::memory_order_release);
        if (!PostQueuedCompletionStatus(this->iocp_, 0, 0, nullptr)) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    int32_t C_ReactorIOCP::TranslateError(const socket_t fd, WSAOVERLAPPED* operation) noexcept
    {
        DWORD flags = 0, transferred = 0;
        if (WSAGetOverlappedResult(fd, operation, &transferred, false, &flags)) {
            return ERROR_SUCCESS;
        }

        if (const auto err = WSAGetLastError(); err != WSAENOTSOCK) {
            return err;
        }

        if (!GetOverlappedResult(reinterpret_cast<HANDLE>(fd), operation, &transferred, false)) {
            return static_cast<int32_t>(GetLastError());
        }

        return ERROR_SUCCESS;
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

    etl::expected<void, int32_t> C_ReactorIOCP::ProceedOperations(HANDLE iocp, uint32_t timeout) noexcept
    {
        OVERLAPPED_ENTRY entries[32]; ULONG fetched = 0;

        if (const bool status = GetQueuedCompletionStatusEx(iocp, entries, std::size(entries), &fetched, timeout, false); !status) {
            const auto err = GetLastError();
            return (err == WAIT_TIMEOUT) ? etl::expected<void, int32_t>() : etl::unexpected(static_cast<int32_t>(err));
        }

        for (const auto& entry : etl::span{entries, fetched}) {
            if (!entry.lpOverlapped) {
                continue;
            }

            int32_t ioError = ERROR_SUCCESS;
            if (entry.lpCompletionKey && !NT_SUCCESS(entry.lpOverlapped->Internal)) {
                ioError = TranslateError(entry.lpCompletionKey, entry.lpOverlapped);
            }

            static_cast<void>(reinterpret_cast<operation_t*>(entry.lpOverlapped)->complete(entry.dwNumberOfBytesTransferred, ioError));
        }

        return {};
    }
}
