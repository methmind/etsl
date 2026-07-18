//
// Created by sexey on 29.05.2026.
//
export module socket.raii;
import socket.types;
import socket.independent;

export namespace etsl
{
    class C_Socket
    {
    public:
        ~C_Socket() noexcept { dispose(); }

        explicit C_Socket(socket_t sock = INVALID_SOCKET_VALUE) noexcept : sock_(sock) {}

        C_Socket(const C_Socket&) = delete;
        C_Socket& operator=(const C_Socket&) = delete;

        C_Socket(C_Socket&& other) noexcept : sock_(other.sock_)
        {
            other.sock_ = INVALID_SOCKET_VALUE;
        }

        C_Socket& operator=(C_Socket&& other) noexcept
        {
            if (this != &other) {
                dispose();
                this->sock_ = other.sock_;
                other.sock_ = INVALID_SOCKET_VALUE;
            }

            return *this;
        }

        socket_t get() const noexcept { return this->sock_; }

        socket_t release() noexcept
        {
            const socket_t temp = this->sock_;
            this->sock_ = INVALID_SOCKET_VALUE;

            return temp;
        }

        bool is_valid() const noexcept { return this->sock_ != INVALID_SOCKET_VALUE; }

        void dispose() const noexcept
        {
            if (!is_valid()) {
                return;
            }

            CloseSocket(this->sock_);
        }

        explicit operator bool() const noexcept { return is_valid(); }
    private:
        socket_t sock_;
    };
}