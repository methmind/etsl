//
// Created by sexey on 23.05.2026.
//
module;

export module event_loop;

import event_loop.trait;

#if defined(_WIN32)
import event_loop.iocp;
export namespace etsl { using C_EventLoop = C_EventLoopIOCP; }
#elif defined(__linux__)
#error "Linux event loop is not implemented yet"
#else
#error "Unsupported platform"
#endif

namespace etsl
{
    static_assert(EventLoopTrait<C_EventLoop>, "C_EventLoop must satisfy event loop trait");

    static_assert(noexcept(C_EventLoop()), "C_EventLoop constructor must be noexcept");
}
