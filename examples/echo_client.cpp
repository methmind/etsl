//
// echo_client — нагрузочный клиент эхо-сервера (ROADMAP 2.5).
//
// Запуск: echo_client [host=127.0.0.1] [port=3730] [mib=64] [connections=1]
//   Каждое соединение шлёт mib МиБ детерминированного потока и сверяет эхо
//   байт-в-байт: потеря, дубль или рассинхрон ловятся по первому расхождению.
//
// Сторожевой таймер — периодический: он перевзводится из собственного колбэка
// одним schedule(), а снимается идемпотентным unschedule() (проверка «взведён
// ли» не нужна). Он же закрывает случай «сервера нет и коннект висит».
// Колбэк — C_Timer::callback_t, то есть void(C_Timer&): сработавший таймер
// приходит аргументом, поэтому перевзвод не требует ссылки на поле-таймер.
//
// Код платформенно-нейтрален: только import etsl и ETL, без заголовков ОС.
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include <etl/chrono.h>
#include <etl/expected.h>
#include <etl/span.h>

import etsl;

namespace
{
    constexpr uint16_t DEFAULT_PORT = 3730;
    constexpr uint32_t DEFAULT_MIB = 64;

    constexpr size_t MAX_CONNECTIONS = 8;
    constexpr size_t TX_SLOTS = 4;          // окно: send-операций в полёте на соединение
    constexpr size_t TX_SLOT_SIZE = 16 * 1024;
    constexpr size_t RX_BUFFER_SIZE = 16 * 1024;

    constexpr etsl::timer_duration_t TICK_PERIOD = etl::chrono::seconds(1);
    constexpr uint32_t STALL_LIMIT_TICKS = 10; // столько тиков без прогресса — провал

    // Байт потока по смещению: хеш номера 8-байтового блока, зависящий от
    // seed соединения. Сдвиг, потеря или чужие данные дают расхождение.
    uint8_t PatternByte(const uint32_t seed, const uint64_t offset) noexcept
    {
        uint64_t x = (offset >> 3) ^ (static_cast<uint64_t>(seed) << 40);
        x *= 0x9E3779B97F4A7C15ull;
        x ^= x >> 31;
        return static_cast<uint8_t>(x >> ((offset & 7) * 8));
    }

    uint64_t ElapsedMs(const etsl::time_point_t& from, const etsl::time_point_t& to) noexcept
    {
        return static_cast<uint64_t>(etl::chrono::duration_cast<etl::chrono::milliseconds>(to - from).count());
    }

    class C_EchoApp;

    struct tx_slot_s : etsl::send_operation_t
    {
        uint8_t data[TX_SLOT_SIZE]{};
        bool busy{false};
    };

    class C_EchoClient
    {
    public:
        using connection_t = etsl::C_TCPConnection<C_EchoClient>;

        C_EchoClient() noexcept = default;
        C_EchoClient(const C_EchoClient&) = delete;
        C_EchoClient& operator=(const C_EchoClient&) = delete;

        void bind(C_EchoApp& app, etsl::C_Reactor& reactor, const uint32_t seed, const uint64_t total) noexcept
        {
            this->app_ = &app;
            this->seed_ = seed;
            this->total_ = total;
            this->connection_.emplace(reactor, *this);
        }

        [[nodiscard]] etl::expected<void, int32_t> start(const etsl::C_Address& addr) noexcept
        {
            if (const auto err = this->connection_->connect(addr); !err) {
                return err; // синхронный отказ — колбэков не будет
            }

            this->running_ = true;
            return {};
        }

        void abort() noexcept
        {
            if (this->running_) {
                this->connection_->dispose();
            }
        }

        [[nodiscard]] bool running() const noexcept { return this->running_; }
        [[nodiscard]] bool succeeded() const noexcept { return this->verified_ == this->total_ && !this->failed_; }
        [[nodiscard]] uint64_t progress() const noexcept { return this->sent_ + this->verified_; }
        [[nodiscard]] uint64_t verified() const noexcept { return this->verified_; }

        // ---- делегат C_TCPConnection ----

        void onConnect(const int32_t error) noexcept
        {
            if (error != 0) { // терминально: соединение не состоялось
                std::printf("[%u] connect failed: %d\n", this->seed_, error);
                this->failed_ = true;
                finish();
                return;
            }

            sendMore();
        }

        void onReadyRead() noexcept
        {
            while (true) {
                const auto received = this->connection_->read({this->rx_, sizeof(this->rx_)});
                if (!received) {
                    return; // WSAEWOULDBLOCK — всё прочитано; иначе — сносится
                }

                if (!verify(received.value())) {
                    this->connection_->dispose();
                    return;
                }

                if (this->verified_ == this->total_) {
                    this->connection_->dispose(); // всё вернулось — закрываемся
                    return;
                }
            }
        }

        void onCommit(etsl::C_Reactor::operation_t& operation, const int32_t error) noexcept
        {
            auto& slot = static_cast<tx_slot_s&>(static_cast<etsl::send_operation_t&>(operation));
            slot.busy = false;

            if (error == 0) {
                sendMore();
            }
        }

        void onDisconnect(const int32_t error) noexcept
        {
            // Сервер закрыл раньше, чем вернул весь поток.
            std::printf("[%u] disconnected by peer: %d, verified %llu of %llu\n", this->seed_, error,
                static_cast<unsigned long long>(this->verified_), static_cast<unsigned long long>(this->total_));
            this->failed_ = true;
            finish();
        }

        void onDisposed() noexcept { finish(); }

    private:
        void sendMore() noexcept
        {
            for (auto& slot : this->slots_) {
                if (this->sent_ == this->total_) {
                    return;
                }

                if (slot.busy) {
                    continue;
                }

                const auto left = this->total_ - this->sent_;
                const auto size = static_cast<size_t>(left < TX_SLOT_SIZE ? left : TX_SLOT_SIZE);
                for (size_t i = 0; i < size; ++i) {
                    slot.data[i] = PatternByte(this->seed_, this->sent_ + i);
                }

                slot.busy = true;
                if (const auto err = this->connection_->send({slot.data, size}, slot); !err) {
                    slot.busy = false; // терминально: итог придёт в onDisconnect
                    return;
                }

                this->sent_ += size;
            }
        }

        [[nodiscard]] bool verify(const uint32_t size) noexcept
        {
            if (this->verified_ + size > this->total_) {
                std::printf("[%u] extra bytes: got %llu past the end\n", this->seed_,
                    static_cast<unsigned long long>(this->verified_ + size - this->total_));
                this->failed_ = true;
                return false;
            }

            for (uint32_t i = 0; i < size; ++i) {
                if (this->rx_[i] != PatternByte(this->seed_, this->verified_ + i)) {
                    std::printf("[%u] mismatch at offset %llu\n", this->seed_,
                        static_cast<unsigned long long>(this->verified_ + i));
                    this->failed_ = true;
                    return false;
                }
            }

            this->verified_ += size;
            return true;
        }

        void finish() noexcept;

        C_EchoApp* app_{nullptr};
        std::optional<connection_t> connection_;
        tx_slot_s slots_[TX_SLOTS];
        uint8_t rx_[RX_BUFFER_SIZE]{};

        uint32_t seed_{0};
        uint64_t total_{0};
        uint64_t sent_{0};
        uint64_t verified_{0};
        bool running_{false};
        bool failed_{false};
    };

    class C_EchoApp
    {
    public:
        C_EchoApp(etsl::C_Reactor& reactor, const uint32_t connections, const uint64_t bytesPerConnection) noexcept :
            reactor_(reactor), watchdog_(etsl::C_Timer::callback_t::create<C_EchoApp, &C_EchoApp::onTick>(*this)),
            connections_(connections), bytesPerConnection_(bytesPerConnection)
        {
            for (uint32_t i = 0; i < connections; ++i) {
                this->clients_[i].bind(*this, reactor, i + 1, bytesPerConnection);
            }
        }

        C_EchoApp(const C_EchoApp&) = delete;
        C_EchoApp& operator=(const C_EchoApp&) = delete;

        [[nodiscard]] bool start(const etsl::C_Address& addr) noexcept
        {
            this->startedAt_ = etsl::steady_clock_t::now();
            for (uint32_t i = 0; i < this->connections_; ++i) {
                if (const auto err = this->clients_[i].start(addr); !err) {
                    std::printf("[%u] connect rejected: %d\n", i + 1, err.error());
                    abortAll();
                    return false;
                }
            }

            this->reactor_.schedule(this->watchdog_, TICK_PERIOD);
            return true;
        }

        [[nodiscard]] bool anyRunning() const noexcept
        {
            for (uint32_t i = 0; i < this->connections_; ++i) {
                if (this->clients_[i].running()) {
                    return true;
                }
            }

            return false;
        }

        void onClientFinished() noexcept
        {
            if (anyRunning()) {
                return;
            }

            // ADR-7: все I/O-владельцы отработали — можно останавливать цикл.
            this->finishedAt_ = etsl::steady_clock_t::now();
            this->reactor_.unschedule(this->watchdog_); // идемпотентно
            if (const auto err = this->reactor_.shutdown(); !err) {
                std::printf("reactor shutdown failed: %d\n", err.error());
                this->stalled_ = true;
            }
        }

        [[nodiscard]] int report() const noexcept
        {
            uint64_t verified = 0;
            bool ok = !this->stalled_;
            for (uint32_t i = 0; i < this->connections_; ++i) {
                verified += this->clients_[i].verified();
                ok = ok && this->clients_[i].succeeded();
            }

            const auto ms = ElapsedMs(this->startedAt_, this->finishedAt_);
            const auto mib = static_cast<double>(verified) / (1024.0 * 1024.0);
            std::printf("%s: %u connection(s), %llu of %llu bytes echoed intact in %llu ms (%.1f MiB/s)\n",
                ok ? "PASS" : "FAIL", this->connections_,
                static_cast<unsigned long long>(verified),
                static_cast<unsigned long long>(this->bytesPerConnection_ * this->connections_),
                static_cast<unsigned long long>(ms), ms ? mib * 1000.0 / static_cast<double>(ms) : 0.0);

            return ok ? 0 : 1;
        }

    private:
        // Раз в TICK_PERIOD: нет прогресса STALL_LIMIT_TICKS подряд — сносим всё.
        void onTick(etsl::C_Timer& timer) noexcept
        {
            uint64_t progress = 0;
            for (uint32_t i = 0; i < this->connections_; ++i) {
                progress += this->clients_[i].progress();
            }

            this->stallTicks_ = (progress == this->lastProgress_) ? this->stallTicks_ + 1 : 0;
            this->lastProgress_ = progress;

            if (this->stallTicks_ >= STALL_LIMIT_TICKS) {
                std::printf("no progress for %llu ms — aborting\n",
                    static_cast<unsigned long long>(TICK_PERIOD.count()) * STALL_LIMIT_TICKS);
                this->stalled_ = true;
                abortAll();
                return;
            }

            // Таймер уже снят очередью перед колбэком — просто ставим заново.
            // Перевзводим именно сработавший (он же watchdog_) — из аргумента.
            this->reactor_.schedule(timer, TICK_PERIOD);
        }

        void abortAll() noexcept
        {
            for (uint32_t i = 0; i < this->connections_; ++i) {
                this->clients_[i].abort();
            }
        }

        etsl::C_Reactor& reactor_;
        etsl::C_Timer watchdog_;
        C_EchoClient clients_[MAX_CONNECTIONS];

        uint32_t connections_;
        uint64_t bytesPerConnection_;
        etsl::time_point_t startedAt_{};
        etsl::time_point_t finishedAt_{};
        uint64_t lastProgress_{0};
        uint32_t stallTicks_{0};
        bool stalled_{false};
    };

    void C_EchoClient::finish() noexcept
    {
        if (!this->running_) {
            return;
        }

        this->running_ = false;
        this->app_->onClientFinished();
    }

    // Объекты крупные (буферы соединений) — держим их в статической памяти.
    etsl::C_Reactor reactor;
    std::optional<C_EchoApp> app;
}

int main(int argc, char** argv)
{
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const auto port = static_cast<uint16_t>(argc > 2 ? std::strtoul(argv[2], nullptr, 10) : DEFAULT_PORT);
    const auto mib = static_cast<uint32_t>(argc > 3 ? std::strtoul(argv[3], nullptr, 10) : DEFAULT_MIB);
    const auto connections = static_cast<uint32_t>(argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 1);

    if (mib == 0 || connections == 0 || connections > MAX_CONNECTIONS) {
        std::printf("usage: echo_client [host] [port] [mib>0] [connections 1..%u]\n",
            static_cast<unsigned>(MAX_CONNECTIONS));
        return 2;
    }

    if (!etsl::Initialize()) {
        std::printf("network subsystem init failed\n");
        return 1;
    }

    if (const auto err = reactor.initialize(); !err) {
        std::printf("reactor init failed: %d\n", err.error());
        return 1;
    }

    etsl::C_Address addr;
    if (const auto err = addr.initialize(host, port); !err) {
        std::printf("bad address %s: %d\n", host, err.error());
        return 1;
    }

    std::printf("echo_client: %s:%u, %u MiB x %u connection(s)\n", host, port, mib, connections);

    app.emplace(reactor, connections, static_cast<uint64_t>(mib) * 1024 * 1024);
    if (!app->start(addr)) {
        if (app->anyRunning()) {
            reactor.run(); // дожидаемся терминальных колбэков уже запущенных соединений
        }

        return 1;
    }

    reactor.run();
    return app->report();
}
