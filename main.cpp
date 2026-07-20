#include <cassert>
#include <windows.h>

import reactor;
import smoke_loopback;

DWORD GetHandlesCount() noexcept
{
    DWORD count;
    GetProcessHandleCount((HANDLE)-1, &count);
    return count;
}

int main()
{
    etsl::C_Reactor reactor;
    if (const auto res = reactor.initialize(); !res) {
        return res.error();
    }

    auto prev = GetHandlesCount();
    smoke_loopback_test(reactor);
    auto after = GetHandlesCount();

    assert(prev == after && "Handle leaked!");

    reactor.run();
    return 0;
}
