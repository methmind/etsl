//
// Created by sexey on 23.05.2026.
//
module;

export module reactor;

import reactor.trait;

#if defined(_WIN32)
import reactor.iocp;
export namespace etsl { using C_Reactor = C_ReactorIOCP; }
#elif defined(__linux__)
#error "Linux reactor is not implemented yet"
#else
#error "Unsupported platform"
#endif

namespace etsl
{
    static_assert(ReactorTrait<C_Reactor>, "C_Reactor must satisfy event loop trait");

    static_assert(noexcept(C_Reactor()), "C_Reactor constructor must be noexcept");
}
