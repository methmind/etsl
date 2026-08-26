//
// Created by sexey on 26.08.2026.
//
export module etsl.init:win;

import :wsa;

export namespace etsl
{
    bool Initialize()
    {
        static C_WSAInitializer wsa;
        if (const auto err = wsa.initialize(); !err) {
            return false;
        }

        return true;
    }
}
