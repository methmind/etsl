//
// Created by sexey on 23.05.2026.
//
module;
#include <windows.h>
#include "etl/atomic.h"
#include "etl/expected.h"

export module event_loop.iocp;

export namespace etsl
{
    class C_EventLoopIOCP
    {
    public:
        ~C_EventLoopIOCP() noexcept;
        C_EventLoopIOCP() noexcept : halt_(false), iocp_(nullptr) {}

        etl::expected<void, int32_t> initialize() noexcept;

        void run() noexcept;

        void shutdown() noexcept;
    private:
        etl::atomic<bool> halt_;
        HANDLE iocp_;
    };
}
