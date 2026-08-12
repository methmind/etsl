//
// Created by sexey on 29.05.2026.
//
module;
#include <util/noncopyable.h>

export module socket:raii;

import net;

export namespace etsl
{
    class C_Socket
    {
    public:
        ~C_Socket() noexcept { dispose(); }

        explicit C_Socket(socket_t sock = INVALID_SOCKET) noexcept : sock_(sock) {}

        ETSL_NON_COPYABLE(C_Socket);

        C_Socket(C_Socket&& other) noexcept : sock_(other.sock_)
        {
            other.sock_ = INVALID_SOCKET;
        }

        C_Socket& operator=(C_Socket&& other) noexcept
        {
            if (this != &other) {
                dispose();
                this->sock_ = other.sock_;
                other.sock_ = INVALID_SOCKET;
            }

            return *this;
        }

        [[nodiscard]] socket_t get() const noexcept { return this->sock_; }

        [[nodiscard]] socket_t release() noexcept
        {
            const auto temp = this->sock_;
            this->sock_ = INVALID_SOCKET;

            return temp;
        }

        [[nodiscard]] bool is_valid() const noexcept { return this->sock_ != INVALID_SOCKET; }

        void dispose() noexcept
        {
            if (!is_valid()) {
                return;
            }

            Shutdown(this->sock_, socket_shutdown_e::BOTH);
            CloseSocket(this->sock_);

            this->sock_ = INVALID_SOCKET;
        }

        explicit operator bool() const noexcept { return is_valid(); }
    private:
        socket_t sock_;
    };
}
