//
// Created by sexey on 26.08.2026.
//
export module etsl.init;

#if defined(_WIN32)
export import :win;
#elif defined(__linux__)
export namespace etsl { bool Initialize() { return true; } }
#else
#error "Unsupported platform"
#endif
