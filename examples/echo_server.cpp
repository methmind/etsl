//
// echo_server — эхо-сервер на C_TCPAcceptor + C_TCPConnection (ROADMAP 2.5).
//
// Запуск: echo_server [port=3730] [sessions=0]
//   sessions — сколько сессий обслужить до выхода (0 — работать бесконечно).
//
// Кроме акцептора и эха пример показывает две вещи из текущего API:
//   * пауза чтения (3.3). Когда все send-слоты в полёте, прочитанное класть
//     некуда; без паузы проба чтения перевзводится вхолостую (ADR-2: на
//     неотвечающем пире — 16 млн пустых onReadyRead за 10 с). Сессия зовёт
//     suspendReading() и снимает паузу, когда слот освободился.
//   * таймеры. У сессии idle-таймаут: schedule() на взведённом таймере
//     переставляет его, поэтому продление — один вызов, без unschedule().
//     Колбэк — C_Timer::callback_t, то есть void(C_Timer&): сработавший таймер
//     приходит ссылкой, так что один обработчик может обслуживать несколько
//     таймеров одного владельца.
//
// Код платформенно-нейтрален: только import etsl и ETL, без заголовков ОС.
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>

#include <etl/chrono.h>
#include <etl/expected.h>
#include <etl/pool.h>
#include <etl/span.h>

import etsl;

namespace
{
    constexpr uint16_t DEFAULT_PORT = 3730;
    constexpr int32_t LISTEN_BACKLOG = 128;

    constexpr size_t ACCEPT_SLOTS = 16;     // взведённых AcceptEx одновременно
    constexpr size_t MAX_SESSIONS = 16;     // одновременно обслуживаемых клиентов
    constexpr size_t ECHO_SLOTS = 4;        // send-операций в полёте на сессию
    constexpr size_t ECHO_SLOT_SIZE = 16 * 1024;

    // Столько времени без единого байта в обе стороны — сессия закрывается.
    constexpr etsl::timer_duration_t IDLE_TIMEOUT = etl::chrono::seconds(30);

    class C_EchoServer;

    // Адрес пира без заголовков ОС: у sockaddr_in и на Windows, и на Linux
    // порт лежит в байтах [2..3], IPv4-адрес — в [4..7] (сетевой порядок).
    void PrintAddress(const etsl::C_Address& addr) noexcept
    {
        if (addr.size() != sizeof(etsl::os_sockaddr_in_t)) {
            std::printf("<non-ipv4>");
            return;
        }

        const auto* raw = reinterpret_cast<const uint8_t*>(&addr.data());
        std::printf("%u.%u.%u.%u:%u", raw[4], raw[5], raw[6], raw[7], (raw[2] << 8) | raw[3]);
    }

    // Send-операция вместе со своим буфером: пока она в полёте, буфер занят
    // (ADR-4 — контекст живёт до терминального onCommit).
    struct echo_slot_s : etsl::send_operation_t
    {
        uint8_t data[ECHO_SLOT_SIZE]{};
        bool busy{false};
    };

    // Одна принятая сессия. Эхо: прочитать всё доступное в свободные слоты и
    // сразу отправить обратно; TCP сохраняет порядок WSASend (ADR-2).
    class C_EchoSession
    {
    public:
        using connection_t = etsl::C_TCPConnection<C_EchoSession>;

        C_EchoSession() noexcept :
            idleTimer_(etsl::C_Timer::callback_t::create<C_EchoSession, &C_EchoSession::onIdleTimeout>(*this))
        {
        }

        C_EchoSession(const C_EchoSession&) = delete;
        C_EchoSession& operator=(const C_EchoSession&) = delete;

        void bind(C_EchoServer& server, etsl::C_Reactor& reactor) noexcept
        {
            this->server_ = &server;
            this->reactor_ = &reactor;
            this->connection_.emplace(reactor, *this);
        }

        [[nodiscard]] bool active() const noexcept { return this->active_; }

        [[nodiscard]] etl::expected<void, int32_t> start(etsl::C_Socket&& fd) noexcept
        {
            this->echoed_ = 0;
            this->readingPaused_ = false;
            this->active_ = true;
            if (const auto err = this->connection_->adopt(std::move(fd)); !err) {
                // Отказ adopt() чист: сокет уже закрыт, колбэков не будет.
                this->active_ = false;
                return err;
            }

            touch();
            return {};
        }

        void dispose() noexcept { this->connection_->dispose(); }

        // ---- делегат C_TCPConnection ----

        void onConnect(const int32_t error) noexcept
        {
            // Для adopt() не вызывается; на всякий случай — как разрыв.
            if (error != 0) {
                finish(error);
            }
        }

        void onReadyRead() noexcept
        {
            touch();
            pump();
        }

        void onCommit(etsl::C_Reactor::operation_t& operation, const int32_t error) noexcept
        {
            auto& slot = static_cast<echo_slot_s&>(static_cast<etsl::send_operation_t&>(operation));
            slot.busy = false;

            if (error != 0) {
                return; // соединение уже сносится, итог придёт терминальным колбэком
            }

            touch();
            if (this->readingPaused_) {
                resumeReading(); // слот освободился — снова можно читать
                return;
            }

            pump();
        }

        void onDisconnect(const int32_t error) noexcept { finish(error); }

        void onDisposed() noexcept { finish(0); }

    private:
        // Продление idle-таймаута: schedule() на взведённом таймере снимает его
        // и ставит заново с новым дедлайном, поэтому unschedule() не нужен.
        // При тысячах соединений то же делают «лениво» (libev): хранить
        // lastActivity и перевзводиться на остаток уже в колбэке таймера.
        void touch() noexcept { this->reactor_->schedule(this->idleTimer_, IDLE_TIMEOUT); }

        // Таймер у сессии один, поэтому аргумент не нужен: он равен idleTimer_.
        void onIdleTimeout(etsl::C_Timer&) noexcept
        {
            std::printf("session idle for %llu ms — closing\n",
                static_cast<unsigned long long>(IDLE_TIMEOUT.count()));
            this->connection_->dispose();
        }

        void pump() noexcept
        {
            for (auto& slot : this->slots_) {
                if (slot.busy) {
                    continue;
                }

                // WSAEWOULDBLOCK — данных больше нет; EOF/ошибка — соединение
                // уже сносится, итог придёт в onDisconnect.
                const auto received = this->connection_->read({slot.data, sizeof(slot.data)});
                if (!received) {
                    return;
                }

                slot.busy = true;
                if (const auto err = this->connection_->send({slot.data, received.value()}, slot); !err) {
                    slot.busy = false; // отказ взвода терминален: onCommit по слоту не придёт
                    return;
                }

                this->echoed_ += received.value();
            }

            // Дошли до конца — все слоты в полёте, класть прочитанное некуда.
            pauseReading();
        }

        void pauseReading() noexcept
        {
            if (this->readingPaused_) {
                return;
            }

            if (const auto err = this->connection_->suspendReading(); !err) {
                return; // не CONNECTED: снос уже идёт, пауза не нужна
            }

            this->readingPaused_ = true;
        }

        void resumeReading() noexcept
        {
            if (!this->readingPaused_) {
                return;
            }

            this->readingPaused_ = false;
            // Данные, пришедшие на паузе, доставляются сразу — придёт onReadyRead.
            // Отказ терминален: соединение сносится, итог придёт колбэком.
            static_cast<void>(this->connection_->resumeReading());
        }

        void finish(int32_t error) noexcept;

        C_EchoServer* server_{nullptr};
        etsl::C_Reactor* reactor_{nullptr};
        std::optional<connection_t> connection_;
        etsl::C_Timer idleTimer_;
        echo_slot_s slots_[ECHO_SLOTS];
        uint64_t echoed_{0};
        bool active_{false};
        bool readingPaused_{false};
    };

    class C_EchoServer
    {
    public:
        using acceptor_t = etsl::C_TCPAcceptor<C_EchoServer>;

        C_EchoServer(etsl::C_Reactor& reactor, const uint32_t sessionLimit) noexcept :
            reactor_(reactor), acceptor_(reactor, backlog_, *this), sessionLimit_(sessionLimit)
        {
            for (auto& session : this->sessions_) {
                session.bind(*this, reactor);
            }
        }

        C_EchoServer(const C_EchoServer&) = delete;
        C_EchoServer& operator=(const C_EchoServer&) = delete;

        [[nodiscard]] etl::expected<void, int32_t> start(const etsl::C_Address& addr) noexcept
        {
            if (const auto err = this->acceptor_.initialize(addr); !err) {
                return err;
            }

            return this->acceptor_.listen(LISTEN_BACKLOG);
        }

        [[nodiscard]] int32_t exitCode() const noexcept { return this->exitCode_; }

        void onSessionFinished(const uint64_t echoed, const int32_t error) noexcept
        {
            ++this->served_;
            std::printf("session closed: echoed %llu bytes, error %d\n",
                static_cast<unsigned long long>(echoed), error);

            if (this->sessionLimit_ != 0 && this->served_ >= this->sessionLimit_ && !this->stopping_) {
                this->stopping_ = true;
                this->acceptor_.dispose();
            }

            tryShutdown();
        }

        // ---- делегат C_TCPAcceptor ----

        void onIncoming(etsl::C_Socket fd, const etsl::C_Address& remoteAddr) noexcept
        {
            std::printf("accepted ");
            PrintAddress(remoteAddr);

            for (auto& session : this->sessions_) {
                if (session.active()) {
                    continue;
                }

                if (const auto err = session.start(std::move(fd)); !err) {
                    std::printf(" — adopt failed: %d\n", err.error());
                    return;
                }

                std::printf("\n");
                return;
            }

            // Свободной сессии нет: сокет закроется деструктором fd.
            std::printf(" — rejected, all %u sessions busy\n", static_cast<unsigned>(MAX_SESSIONS));
        }

        void onError(const int32_t error) noexcept
        {
            std::printf("acceptor error (non-fatal): %d\n", error);
        }

        void onDisposed(const int32_t reason) noexcept
        {
            if (reason != -1) { // не явный dispose() — акцептор умер сам
                std::printf("acceptor failed: %d\n", reason);
                this->exitCode_ = 1;
                this->stopping_ = true;
                for (auto& session : this->sessions_) {
                    if (session.active()) {
                        session.dispose();
                    }
                }
            }

            this->acceptorDisposed_ = true;
            tryShutdown();
        }

    private:
        // ADR-7: цикл останавливаем только когда все I/O-владельцы отработали.
        void tryShutdown() noexcept
        {
            if (!this->acceptorDisposed_) {
                return;
            }

            for (const auto& session : this->sessions_) {
                if (session.active()) {
                    return;
                }
            }

            if (const auto err = this->reactor_.shutdown(); !err) {
                std::printf("reactor shutdown failed: %d\n", err.error());
                this->exitCode_ = 1;
            }
        }

        etsl::C_Reactor& reactor_;
        etl::pool<acceptor_t::accept_operation_t, ACCEPT_SLOTS> backlog_{};
        acceptor_t acceptor_;
        C_EchoSession sessions_[MAX_SESSIONS];

        uint32_t sessionLimit_;
        uint32_t served_{0};
        int32_t exitCode_{0};
        bool stopping_{false};
        bool acceptorDisposed_{false};
    };

    void C_EchoSession::finish(const int32_t error) noexcept
    {
        if (!this->active_) {
            return;
        }

        // unschedule() идемпотентен: невзведённый таймер — тихий no-op.
        this->reactor_->unschedule(this->idleTimer_);
        this->active_ = false;
        this->readingPaused_ = false;
        this->server_->onSessionFinished(this->echoed_, error);
    }

    // Объекты крупные (буферы сессий) — держим их в статической памяти.
    etsl::C_Reactor reactor;
    std::optional<C_EchoServer> server;
}

int main(int argc, char** argv)
{
    const auto port = static_cast<uint16_t>(argc > 1 ? std::strtoul(argv[1], nullptr, 10) : DEFAULT_PORT);
    const auto sessionLimit = static_cast<uint32_t>(argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 0);

    if (!etsl::Initialize()) {
        std::printf("network subsystem init failed\n");
        return 1;
    }

    if (const auto err = reactor.initialize(); !err) {
        std::printf("reactor init failed: %d\n", err.error());
        return 1;
    }

    etsl::C_Address addr;
    if (const auto err = addr.initialize("0.0.0.0", port); !err) {
        std::printf("bad address: %d\n", err.error());
        return 1;
    }

    server.emplace(reactor, sessionLimit);
    if (const auto err = server->start(addr); !err) {
        std::printf("listen on port %u failed: %d\n", port, err.error());
        return 1; // акцептор после отказа чист — колбэков не будет
    }

    std::printf("echo_server: listening on port %u, session limit %u, idle timeout %llu ms\n",
        port, sessionLimit, static_cast<unsigned long long>(IDLE_TIMEOUT.count()));
    reactor.run();

    return server->exitCode();
}
