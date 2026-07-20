//
// Created by sexey on 20.07.2026.
//
#if defined(_WIN32)
#include <cstdint>
#include <windows.h>

extern "C" uint64_t etl_get_steady_clock()
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);

    uint64_t qpc = counter.QuadPart;
    uint64_t freq = frequency.QuadPart;

    uint64_t seconds = qpc / freq;
    uint64_t remainder = qpc % freq;

    return (seconds * 1000000000ULL) + ((remainder * 1000000000ULL) / freq);
}

extern "C" uint64_t etl_get_high_resolution_clock()
{
    return etl_get_steady_clock();
}
#elif defined(_linux)
#include <time.h>

extern "C" uint64_t etl_get_steady_clock()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL) + ts.tv_nsec;
}

extern "C" uint64_t etl_get_high_resolution_clock()
{
    return etl_get_steady_clock();
}
#else
#error "Unsupported platform"
#endif