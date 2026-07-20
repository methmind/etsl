//
// Created by sexey on 19.07.2026.
//
module;
#include <thread>
#include <winsock2.h>
#include <cassert>
#include <iostream>
#include <etl/delegate.h>

module smoke_loopback;

import socket.events;

struct C_TestConnection {
    etsl::C_Reactor::reg_info_t reg{};
    etsl::C_Reactor* loop{nullptr};

    bool read_triggered{false};
    bool closed_triggered{false};

    void on_event(etsl::socket_event_flags_e events, int32_t error) noexcept {
        if (events == etsl::socket_event_flags_e::EV_READ) {
            char buf[256];
            int res = recv(reg.fd, buf, sizeof(buf), 0);
            if (res > 0) {
                read_triggered = true;
                std::cout << "[Server] EV_READ: received " << res << " bytes.\n";
            }
        }

        if (events == etsl::socket_event_flags_e::EV_CLOSED) {
            closed_triggered = true;
            std::cout << "[Server] EV_CLOSED: peer disconnected (error code: " << error << ").\n";
            // Останавливаем цикл реактора
            loop->shutdown();
        }

        if (events == etsl::socket_event_flags_e::EV_ERROR) {
            std::cerr << "[Server] EV_ERROR: " << error << '\n';
        }
    }
};

void smoke_loopback_test(etsl::C_Reactor& loop) noexcept
{
    // 1. Подготавливаем серверный сокет в главном потоке, чтобы знать порт
    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(server_fd != INVALID_SOCKET);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; // ОС выдаст свободный порт

    bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(server_fd, 1);

    int addr_len = sizeof(addr);
    getsockname(server_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);

    static C_TestConnection conn;
    conn.loop = &loop;

    // 2. Запускаем серверный поток (Реактор)
    std::thread server_thread([&]() {
        // Блокируемся, пока клиент не подключится
        SOCKET accepted_fd = accept(server_fd, nullptr, nullptr);
        assert(accepted_fd != INVALID_SOCKET);

        // Биндим сокет к реактору
        auto cb = etsl::socket_event_callback_t::create<C_TestConnection, &C_TestConnection::on_event, conn>();
        loop.attach(accepted_fd, cb, conn.reg);

        // Запускаем обработку событий (заблокирует поток до вызова shutdown)
        loop.run();

        // Корректная очистка после выхода из цикла
        loop.detach(accepted_fd, conn.reg);
        closesocket(accepted_fd);
    });

    Sleep(200);

    // 3. Выполняем клиентский сценарий в главном потоке
    SOCKET client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    // Отправляем данные (триггер для EV_READ)
    const char* msg = "Smoke test payload";
    send(client_fd, msg, static_cast<int>(strlen(msg)), 0);

    // Даем серверу немного времени на обработку, чтобы не смешать payload и FIN
    Sleep(100);

    // Закрываем сокет (триггер для EV_CLOSED)
    closesocket(client_fd);

    // 4. Ждем завершения фонового потока (он завершится, как только отработает shutdown())
    server_thread.join();
    closesocket(server_fd);

    // Проверяем, что все коллбэки отработали
    assert(conn.read_triggered && "EV_READ test failed");
    assert(conn.closed_triggered && "EV_CLOSED test failed");
}