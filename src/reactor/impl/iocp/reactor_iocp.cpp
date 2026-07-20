//
// Created by sexey on 23.05.2026.
//
module;
#include <winsock2.h>
#include <windows.h>
#include <cstdint>

#include "etl/expected.h"

module reactor.iocp;

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

        this->iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (!this->iocp_) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    etl::expected<void, int32_t> C_ReactorIOCP::attach(const socket_t fd, const socket_event_callback_t& cb, reg_info_t& reg) noexcept
    {
        reg = {.fd = fd, .cb = cb};
        if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(fd), this->iocp_,
            static_cast<ULONG_PTR>(iocp_code_e::IO), 0)) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return RearmRead(&reg);
    }

    etl::expected<void, int32_t> C_ReactorIOCP::detach(socket_t fd, reg_info_t& reg) noexcept
    {
        reg.is_closing = true;
        if (!CancelIoEx(reinterpret_cast<HANDLE>(fd), &reg)) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    etl::expected<void, int32_t> C_ReactorIOCP::post(task_t* task) noexcept
    {
        if (!PostQueuedCompletionStatus(this->iocp_, 0, static_cast<ULONG_PTR>(iocp_code_e::TASK),
            reinterpret_cast<LPOVERLAPPED>(task))) {
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    etl::expected<void, int32_t> C_ReactorIOCP::addTimer(C_Timer& timer) noexcept
    {
        this->timerBucket_.add(timer);
        if (!PostQueuedCompletionStatus(this->iocp_, 0, static_cast<ULONG_PTR>(iocp_code_e::ADD_TIMER), nullptr)) {
            removeTimer(timer);
            return etl::unexpected(static_cast<int32_t>(GetLastError()));
        }

        return {};
    }

    void C_ReactorIOCP::removeTimer(C_Timer& timer) noexcept
    {
        this->timerBucket_.remove(timer);
    }

    void C_ReactorIOCP::run() noexcept
    {
        while (this->halt_.load() == false) {
            DWORD bytesTransferred = 0;
            ULONG_PTR completionKey = 0;
            WSAOVERLAPPED* overlapped = nullptr;

            const auto ioStatus = GetQueuedCompletionStatus(this->iocp_, &bytesTransferred, &completionKey,
                &overlapped, this->timerBucket_.nextTimeout(clock_t::now())
            );

            // Fire expired timers on every wake-up: IO completion, posted task or wait timeout.
            const auto currentTime = clock_t::now();
            while (const auto timer = this->timerBucket_.pop(currentTime)) {
                timer->execute();
            }

            if (!overlapped) {
                continue; // Wait timeout or a posted wake-up (SHUTDOWN/ADD_TIMER) — nothing to dispatch.
            }

            if (completionKey == static_cast<ULONG_PTR>(iocp_code_e::TASK)) {
                const auto taskInfo = reinterpret_cast<task_t*>(overlapped);
                taskInfo->execute(taskInfo);
                continue;
            }

            const auto regInfo = static_cast<reg_info_t*>(overlapped);
            if (regInfo->is_closing) {
                continue; // Пришел aborted-completion от CancelIoEx. Теперь regInfo можно безопасно удалять.
            }

            DWORD resultFlags = 0;
            if (!ioStatus && !WSAGetOverlappedResult(regInfo->fd, overlapped, &bytesTransferred, FALSE, &resultFlags)) {
                regInfo->cb(socket_event_flags_e::EV_ERROR, WSAGetLastError());
                continue;
            }

            char peek;
            if (const auto recvRes = recv(regInfo->fd, &peek, 1, MSG_PEEK); recvRes > 0) {
                regInfo->cb(socket_event_flags_e::EV_READ, 0);
                if (const auto rearmErr = RearmRead(regInfo); !rearmErr) {
                    regInfo->cb(socket_event_flags_e::EV_ERROR, rearmErr.error());
                }
            }
            else if (recvRes == 0) {
                regInfo->cb(socket_event_flags_e::EV_CLOSED, 0);
            }
            else { // recvRes < 0
                switch (const auto wsaErr = WSAGetLastError()) {
                    case WSAEWOULDBLOCK:
                        if (const auto rearmErr = RearmRead(regInfo); !rearmErr) {
                            regInfo->cb(socket_event_flags_e::EV_ERROR, rearmErr.error());
                        }
                        break;
                    case WSAECONNRESET:
                    case WSAECONNABORTED:
                        regInfo->cb(socket_event_flags_e::EV_CLOSED, wsaErr);
                        break;
                    default:
                        regInfo->cb(socket_event_flags_e::EV_ERROR, wsaErr);
                        break;
                }
            }
        }
    }

    void C_ReactorIOCP::shutdown() noexcept
    {
        this->halt_.store(true);
        PostQueuedCompletionStatus(this->iocp_, 0, static_cast<ULONG_PTR>(iocp_code_e::SHUTDOWN), nullptr);
    }

    etl::expected<void, int32_t> C_ReactorIOCP::RearmRead(reg_info_t* reg) noexcept
    {
        if (reg->is_closing) {
            return {};
        }

        memset(reg, 0, sizeof(WSAOVERLAPPED));
        DWORD flags = 0;
        WSABUF buf{};

        if (const auto res = WSARecv(reg->fd, &buf, 1, nullptr, &flags,
            reg, nullptr); res == NO_ERROR) {
            return {};
        }

        const auto err = WSAGetLastError();
        if (err == WSA_IO_PENDING) {
            return {};
        }

        return etl::unexpected(err);
    }
}
