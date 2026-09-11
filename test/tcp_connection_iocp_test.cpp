#include <gtest/gtest.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <etl/chrono.h>
#include <etl/pool.h>
#include <etl/span.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

import etsl;

namespace {

// The library no longer initializes Winsock implicitly: etsl::Initialize() is
// the user-side entry point. Run it once for the whole test process.
class WinEnvironment final : public ::testing::Environment
{
public:
    void SetUp() override
    {
        // Unbuffered stdout: gtest messages survive an abort() mid-test.
        (void)setvbuf(stdout, nullptr, _IONBF, 0);
        ASSERT_TRUE(etsl::Initialize());
    }
};

[[maybe_unused]] static const auto* const g_winEnv =
    ::testing::AddGlobalTestEnvironment(new WinEnvironment);

// Reserves a free loopback port by binding an ephemeral socket and releasing
// it. Used both to give the acceptor a deterministic port (the bound gateway
// is not observable through the public API) and to produce a dead port for
// connect-refused scenarios.
[[nodiscard]] bool ReservePort(uint16_t& port) noexcept
{
    const SOCKET probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ::closesocket(probe);
        return false;
    }

    int addrLen = sizeof(addr);
    if (::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR) {
        ::closesocket(probe);
        return false;
    }

    ::closesocket(probe);
    port = ntohs(addr.sin_port);
    return port != 0;
}

// Blocking connect to a live listener, then nonblocking mode: a socket ready
// for C_TCPConnectionIOCP::adopt() (acceptor hands out sockets like this).
[[nodiscard]] bool MakeConnectedSocket(const uint16_t port, etsl::C_Socket& out) noexcept
{
    const SOCKET raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (raw == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr) != 1
        || ::connect(raw, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == SOCKET_ERROR) {
        ::closesocket(raw);
        return false;
    }

    u_long nonblocking = 1;
    if (::ioctlsocket(raw, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        ::closesocket(raw);
        return false;
    }

    out = etsl::C_Socket(raw);
    return true;
}

class TCPConnectionIOCPTest;

// One accepted connection owned by the test side. The acceptor hands the
// socket over in onIncoming, the slot adopts it and behaves according to the
// per-test configuration: send a payload right away, capture what the client
// sends, echo by tag, close gracefully or abortively.
class PeerDelegate
{
public:
    using Conn = etsl::C_TCPConnection<PeerDelegate>;

    struct Config
    {
        // Bytes pushed to the client right after adopt.
        const uint8_t* sendOnAdopt = nullptr;
        size_t sendOnAdoptLen = 0;
        // Keep the accepted fd raw and close it with SO_LINGER{1,0} (RST)
        // instead of adopting it into a library connection.
        bool abortiveClose = false;
        // Close right after adopt (gracefully, or abortively in the raw mode).
        bool closeOnAdopt = false;
        // Capture incoming bytes into a caller-owned buffer.
        char* captureBuf = nullptr;
        int captureCap = 0;
        // Once captureCap bytes are captured, report to the fixture
        // (used to dispose the client only after the payload is received).
        bool captureReport = false;
        // Capture a 4-byte tag, then let the fixture route an echo payload.
        bool tagEcho = false;
        // Graceful dispose after the first successful send commit.
        bool closeOnCommit = false;
    };

    explicit PeerDelegate(TCPConnectionIOCPTest& owner) noexcept
        : owner_(owner) {}

    PeerDelegate(const PeerDelegate&) = delete;
    PeerDelegate& operator=(const PeerDelegate&) = delete;

    void Adopt(etsl::C_Socket fd) noexcept;
    void Send(const uint8_t* data, size_t len) noexcept;
    void Close() noexcept;
    void Abort() noexcept;
    void Release() noexcept;

    [[nodiscard]] bool Used() const noexcept { return adoptedCount > 0; }
    [[nodiscard]] bool IsAborted() const noexcept { return aborted_; }
    [[nodiscard]] bool IsIdle() const noexcept
    {
        return disconnectedCount > 0 || disposedCount > 0 || aborted_;
    }

    void onConnect(int32_t error) noexcept;
    void onReadyRead() noexcept;
    void onCommit(etsl::C_Reactor::operation_t& operation, int32_t error) noexcept;
    void onDisconnect(int32_t error) noexcept;
    void onDisposed() noexcept;

    Config cfg;

    int adoptedCount{0};
    int readyReadCount{0};
    int commitCount{0};
    int disconnectedCount{0};
    int disposedCount{0};
    int32_t lastDisconnectError{0};
    int32_t lastCommitError{0};
    int capturedLen{0};
    bool sendOk{false};

    // Internal 4-byte tag storage for the tagEcho mode.
    char tag[4]{};

private:
    TCPConnectionIOCPTest& owner_;
    std::optional<Conn> conn_;
    etsl::send_operation_t sendOp_{};
    SOCKET rawSocket_{INVALID_SOCKET};
    bool payloadNotified_{false};
    bool aborted_{false};
};

// Second client connection on the same reactor (the fixture itself drives the
// first one). Connects to the acceptor, posts its tag and consumes the echo —
// used to verify per-instance isolation.
class SecondClientDelegate
{
public:
    using Conn = etsl::C_TCPConnection<SecondClientDelegate>;

    explicit SecondClientDelegate(TCPConnectionIOCPTest& owner) noexcept
        : owner_(owner) {}

    SecondClientDelegate(const SecondClientDelegate&) = delete;
    SecondClientDelegate& operator=(const SecondClientDelegate&) = delete;

    void Start(const etsl::C_Address& addr) noexcept;
    void Release() noexcept { conn_.reset(); }

    void onConnect(int32_t error) noexcept;
    void onReadyRead() noexcept;
    void onCommit(etsl::C_Reactor::operation_t& operation, int32_t error) noexcept;
    void onDisconnect(int32_t error) noexcept;
    void onDisposed() noexcept;

    bool startedOk_{false};
    bool sendOk_{false};
    int connectCount_{0};
    int readyReadCount_{0};
    int disconnectCount_{0};
    int disposedCount_{0};
    int commitCount_{0};
    int32_t lastConnectError_{0};
    int32_t lastDisconnectError_{0};
    int32_t lastCommitError_{0};
    char payload_[8]{};
    int payloadLen_{0};

private:
    TCPConnectionIOCPTest& owner_;
    std::optional<Conn> conn_;
    etsl::send_operation_t sendOp_{};
    uint8_t sendBytes_[4]{'2', 'n', 'd', '!'};
};

// A standalone acceptor with a single-slot backlog pool, driven by its own
// delegate type so its counters stay independent from the fixture-owned
// acceptor. Exercises the "last armed operation → teardown" path through
// exactly one pool element.
class SingleSlotDelegate
{
public:
    using Acceptor = etsl::C_TCPAcceptor<SingleSlotDelegate>;

    explicit SingleSlotDelegate(TCPConnectionIOCPTest& owner) noexcept
        : owner_(owner) {}

    SingleSlotDelegate(const SingleSlotDelegate&) = delete;
    SingleSlotDelegate& operator=(const SingleSlotDelegate&) = delete;

    bool Start(const etsl::C_Address& addr) noexcept;
    void Dispose() noexcept;
    void Release() noexcept;

    void onIncoming(etsl::C_Socket fd, const etsl::C_Address& remoteAddr) noexcept;
    void onError(int32_t error) noexcept;
    void onDisposed(int32_t reason) noexcept;

    etl::pool<Acceptor::accept_operation_t, 1> backlog{};
    std::optional<Acceptor> acceptor_;
    etsl::C_Socket held;
    int accepted{0};
    int disposed{0};
    int errors{0};
    bool started{false};
    int32_t lastReason{0};
    int32_t lastError{0};

private:
    TCPConnectionIOCPTest& owner_;
};

// The fixture is the delegate of every side at once: the client connection
// under test, the acceptor and (through PeerDelegate) the accepted peer
// connections. Everything runs on one reactor thread; the loop is stopped
// only after every engaged party has reached a terminal state, so TearDown
// never destroys a busy connection.
class TCPConnectionIOCPTest : public ::testing::Test
{
public:
    using clock_t = etsl::clock_t;
    using timer_callback_t = etsl::timer_callback_t;
    using C_Address = etsl::C_Address;
    using C_Reactor = etsl::C_Reactor;
    using C_Timer = etsl::C_Timer;
    using C_Socket = etsl::C_Socket;
    using send_operation_t = etsl::send_operation_t;
    using Driver = etsl::C_TCPConnection<TCPConnectionIOCPTest>;
    using Acceptor = etsl::C_TCPAcceptor<TCPConnectionIOCPTest>;

    TCPConnectionIOCPTest() noexcept
        : peers_{ {PeerDelegate(*this), PeerDelegate(*this),
                   PeerDelegate(*this), PeerDelegate(*this)} } {}

    [[nodiscard]] C_Reactor& reactor() noexcept { return reactor_; }

    [[nodiscard]] PeerDelegate& peer(const size_t index) noexcept { return peers_[index]; }

    // Creates the acceptor object without initialize/listen (contract tests).
    void EmplaceAcceptor() noexcept
    {
        if (!acceptor_.has_value()) {
            acceptor_.emplace(reactor_, backlog_, *this);
        }
    }

    // Binds and listens on a freshly reserved loopback port.
    [[nodiscard]] bool StartAcceptor() noexcept
    {
        uint16_t port = 0;
        if (!ReservePort(port)) {
            return false;
        }

        acceptorAddress_.emplace();
        if (!acceptorAddress_->initialize("127.0.0.1", port).has_value()) {
            return false;
        }

        EmplaceAcceptor();
        if (!acceptor_->initialize(*acceptorAddress_).has_value()) {
            return false;
        }

        if (!acceptor_->listen(4).has_value()) {
            return false;
        }

        acceptorPort_ = port;
        acceptorStarted_ = true;
        return true;
    }

    [[nodiscard]] const C_Address& AcceptorAddress() const noexcept { return *acceptorAddress_; }
    [[nodiscard]] uint16_t AcceptorPort() const noexcept { return acceptorPort_; }

    [[nodiscard]] bool MakeAddress(const uint16_t port, C_Address& out) noexcept
    {
        return out.initialize("127.0.0.1", port).has_value();
    }

    // Starts the second client against the acceptor address.
    void StartSecondClient(const C_Address& addr) noexcept
    {
        auxEngaged_ = true;
        second_.Start(addr);
    }

    // Tag router for the tagEcho peer mode: reads the captured 4-byte tag and
    // sends the matching echo payload, closing the peer after the commit.
    void EchoByTag(PeerDelegate& p) noexcept
    {
        static constexpr uint8_t kEchoA[5]{'e', 'c', 'h', 'o', 'A'};
        static constexpr uint8_t kEchoB[5]{'e', 'c', 'h', 'o', 'B'};

        if (p.capturedLen < 4) {
            return;
        }

        const uint8_t* payload = nullptr;
        if (std::memcmp(p.tag, "FIX1", 4) == 0) {
            payload = kEchoA;
        } else if (std::memcmp(p.tag, "2nd!", 4) == 0) {
            payload = kEchoB;
        }

        if (payload == nullptr) {
            return;
        }

        p.cfg.closeOnCommit = true;
        p.Send(payload, sizeof(kEchoA));
    }

    // A peer slot finished capturing its expected payload. Used to dispose the
    // client only after the peer actually received the data — an immediate
    // dispose-on-commit races with the peer's read probe on loopback and can
    // abort the delivery before onReadyRead fires.
    void OnPeerCaptured(PeerDelegate& /*p*/) noexcept
    {
        if (disposeOnPeerCapture_) {
            driver_->dispose();
        }
    }

    // ---- finish gate -------------------------------------------------------
    //
    // Every terminal callback of the client under test (except a reconnect
    // relaunch) requests the finish; acceptor-only tests request it manually
    // from the test body. The loop stops only when the peers and the second
    // client are drained and the acceptor has completed its own dispose.

    void RequestFinish() noexcept
    {
        finishRequested_ = true;
        TryFinish();
    }

    void NotifyAuxIdle() noexcept
    {
        auxIdle_ = true;
        TryFinish();
    }

    void NotifySecondaryDisposed() noexcept
    {
        TryFinish();
    }

    void NotifyPeerIdle() noexcept
    {
        TryFinish();
    }

    // ---- connection delegate ----------------------------------------------

    void onReadyRead() noexcept
    {
        readyReadCount_++;

        if (captureReadyReadPayload_) {
            capturedPayloadLen_ = 0;
            while (capturedPayloadLen_ < static_cast<int>(sizeof(capturedPayload_))) {
                // value_or(0): operator* is broken under -fno-exceptions; unexpected
                // (WOULDBLOCK drained / EOF / IO error — the latter two start teardown) → 0 → stop.
                const uint32_t n = driver_->read({
                    reinterpret_cast<uint8_t*>(capturedPayload_ + capturedPayloadLen_),
                    static_cast<size_t>(sizeof(capturedPayload_) - capturedPayloadLen_)
                }).value_or(0);
                if (n == 0) {
                    break;
                }
                capturedPayloadLen_ += static_cast<int>(n);
            }
        } else if (consumeReadyRead_) {
            uint8_t buf[64];
            while (true) {
                if (driver_->read({buf, sizeof(buf)}).value_or(0) == 0) {
                    break;
                }
            }
        }

        if (disposeAfterReadyReads_ > 0 && readyReadCount_ >= disposeAfterReadyReads_) {
            driver_->dispose();
            return;
        }

        if (disposeOnReadyRead_) {
            driver_->dispose();
        }
    }

    void onCommit(C_Reactor::operation_t& operation, const int32_t error) noexcept
    {
        commitCount_++;
        lastCommitError_ = error;
        lastCommitOperation_ = &operation;

        if (sendOnCommit_ != nullptr && error == 0) {
            auto* next = sendOnCommit_;
            sendOnCommit_ = nullptr;
            const auto again = driver_->send(next->content, *next);
            chainedSendAttempted_ = true;
            chainedSendOk_ = again.has_value();
            if (!again) {
                chainedSendError_ = again.error();
            }
            return;
        }

        if (disposeOnCommit_ && error == 0) {
            driver_->dispose();
            return;
        }

        if (disposeAfterCommits_ > 0 && commitCount_ >= disposeAfterCommits_ && error == 0) {
            driver_->dispose();
        }
    }

    void onConnect(const int32_t error) noexcept
    {
        connectCount_++;
        lastConnectError_ = error;

        if (error == 0 && secondConnectAddress_ != nullptr) {
            const auto secondAttempt = driver_->connect(*secondConnectAddress_);
            secondConnectAttempted_ = true;
            secondConnectOk_ = secondAttempt.has_value();
            if (!secondAttempt) {
                secondConnectError_ = secondAttempt.error();
            }
        }

        if (error != 0 && reconnectOnConnectError_ && reconnectAddress_ != nullptr) {
            reconnectOnConnectError_ = false;
            const auto again = driver_->connect(*reconnectAddress_);
            reconnectAttempted_ = true;
            reconnectOk_ = again.has_value();
            if (!again) {
                reconnectError_ = again.error();
            }
            return;
        }

        if (error == 0 && sendOnConnect_ != nullptr) {
            const auto sent = driver_->send(sendOnConnect_->content, *sendOnConnect_);
            sendOnConnectAttempted_ = true;
            sendOnConnectOk_ = sent.has_value();
            if (!sent) {
                sendOnConnectError_ = sent.error();
            }
            sendOnConnect_ = nullptr;
        }

        if (error == 0 && sendOnConnect2_ != nullptr) {
            const auto sent = driver_->send(sendOnConnect2_->content, *sendOnConnect2_);
            secondSendOnConnectAttempted_ = true;
            secondSendOnConnectOk_ = sent.has_value();
            sendOnConnect2_ = nullptr;
        }

        if (disposeAfterConnects_ > 0 && connectCount_ >= disposeAfterConnects_ && error == 0) {
            driver_->dispose();
            return;
        }

        if (disposeOnConnect_ && error == 0) {
            driver_->dispose();
            return;
        }

        if (scheduleDisposeOnConnectMs_ >= 0 && error == 0) {
            ScheduleDispose(scheduleDisposeOnConnectMs_);
            scheduleDisposeOnConnectMs_ = -1;
            return;
        }

        // A failed connect is a terminal state of the connection object; a
        // successful onConnect is not (the session keeps going).
        if (error != 0) {
            RequestFinish();
        }
    }

    void onDisconnect(const int32_t error) noexcept
    {
        disconnectCount_++;
        lastDisconnectError_ = error;

        if (reconnectOnDisconnect_ && reconnectAddress_ != nullptr) {
            reconnectOnDisconnect_ = false;
            const auto again = driver_->connect(*reconnectAddress_);
            reconnectAttempted_ = true;
            reconnectOk_ = again.has_value();
            if (!again) {
                reconnectError_ = again.error();
            }
            return;
        }

        RequestFinish();
    }

    void onDisposed() noexcept
    {
        disposedCount_++;

        if (reconnectOnDisposed_ && reconnectAddress_ != nullptr && disposedCount_ == 1) {
            reconnectOnDisposed_ = false;
            const auto again = driver_->connect(*reconnectAddress_);
            reconnectAttempted_ = true;
            reconnectOk_ = again.has_value();
            if (!again) {
                reconnectError_ = again.error();
            }
            return;
        }

        RequestFinish();
    }

    // ---- acceptor delegate -------------------------------------------------

    // The acceptor resolves the remote endpoint of every accepted socket; the
    // client side of the loopback is always 127.0.0.1:<ephemeral>, so each
    // delivery must carry a loopback IPv4 address.
    [[nodiscard]] static bool IsLoopbackRemote(const etsl::C_Address& addr) noexcept
    {
        if (addr.size() != sizeof(sockaddr_in)) {
            return false;
        }

        const auto& sa = reinterpret_cast<const sockaddr_in&>(addr.data());
        return sa.sin_family == AF_INET && sa.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    }

    void onIncoming(etsl::C_Socket fd, const etsl::C_Address& remoteAddr) noexcept
    {
        acceptedCount_++;
        if (IsLoopbackRemote(remoteAddr)) {
            loopbackRemoteCount_++;
        }

        for (auto& slot : peers_) {
            if (!slot.Used()) {
                slot.Adopt(std::move(fd));
                break;
            }
        }

        if (disposeFromOnIncoming_) {
            // Reentrant teardown from inside the accept delivery. Mark the
            // dispose as requested so the finish gate does not issue a
            // second one and waits for this one to complete instead.
            acceptorDisposeRequested_ = true;
            acceptor_->dispose();
        }
    }

    void onError(const int32_t error) noexcept
    {
        acceptorErrorCount_++;
        acceptorLastError_ = error;
    }

    void onDisposed(const int32_t reason) noexcept
    {
        acceptorDisposedCount_++;
        acceptorLastReason_ = reason;

        if (reuseAcceptorOnDisposed_ && acceptorDisposedCount_ == 1 && rebornAddress_.has_value()) {
            // The object is expected back at NONE: rebind on a fresh port and
            // relaunch listening right from the terminal callback. The guards
            // inside initialize()/listen() double as the clean-state checks
            // (gateway closed, backlog drained, state and reason cache reset
            // — otherwise WSAEALREADY/WSAEINVAL would fire here instead).
            reuseAcceptorOnDisposed_ = false;
            rebornInitOk_ = acceptor_->initialize(*rebornAddress_).has_value();
            rebornListenOk_ = rebornInitOk_ && acceptor_->listen(4).has_value();
            if (!rebornListenOk_) {
                TryFinish();
                return;
            }

            acceptorDisposeRequested_ = false; // the gate disposes the reborn acceptor later
            rebornConnectOk_ = driver_->connect(*rebornAddress_).has_value();
            return;
        }

        TryFinish();
    }

protected:
    void SetUp() override
    {
        ASSERT_TRUE(reactor_.initialize().has_value());
        driver_.emplace(reactor_, *this);
    }

    void TearDown() override
    {
        if (actionTimer_ && actionTimer_->is_linked()) {
            reactor_.removeTimer(*actionTimer_);
        }
        if (timeoutTimer_ && timeoutTimer_->is_linked()) {
            reactor_.removeTimer(*timeoutTimer_);
        }
        if (stopTimer_ && stopTimer_->is_linked()) {
            reactor_.removeTimer(*stopTimer_);
        }
        actionTimer_.reset();
        timeoutTimer_.reset();
        stopTimer_.reset();
        driver_.reset();
        for (auto& slot : peers_) {
            slot.Release();
        }
        second_.Release();
        singleSlot_.Release();
        acceptor_.reset();
        acceptorAddress_.reset();
        secondConnectAddress_ = nullptr;
        reconnectAddress_ = nullptr;
        sendOnConnect2_ = nullptr;
    }

    void RunReactor(const int32_t timeoutMs = 3000)
    {
        timedOut_ = false;
        timeoutTimer_.emplace(timer_callback_t::create<TCPConnectionIOCPTest,
            &TCPConnectionIOCPTest::OnTimeout>(*this));

        auto deadline = clock_t::now();
        deadline += etl::chrono::duration_cast<clock_t::duration>(etl::chrono::milliseconds(timeoutMs));
        timeoutTimer_->arm(deadline);
        reactor_.addTimer(*timeoutTimer_);

        reactor_.run();

        if (timeoutTimer_ && timeoutTimer_->is_linked()) {
            reactor_.removeTimer(*timeoutTimer_);
        }
        timeoutTimer_.reset();
    }

    void ScheduleDispose(const int32_t delayMs)
    {
        actionTimer_.emplace(timer_callback_t::create<TCPConnectionIOCPTest,
            &TCPConnectionIOCPTest::OnActionTimer>(*this));
        auto deadline = clock_t::now();
        deadline += etl::chrono::duration_cast<clock_t::duration>(etl::chrono::milliseconds(delayMs));
        actionTimer_->arm(deadline);
        reactor_.addTimer(*actionTimer_);
        actionDispose_ = true;
    }

    void PrepareSend(send_operation_t& operation, uint8_t* data, const size_t len) noexcept
    {
        operation.content = etl::span<const uint8_t>{data, len};
        operation.transferred = 0;
    }

protected:
    void TryFinish() noexcept
    {
        if (!finishRequested_ || finished_) {
            return;
        }

        // Every engaged party must be back at a terminal state before the
        // loop stops, otherwise TearDown would destroy busy connections.
        for (const auto& slot : peers_) {
            if (slot.Used() && !slot.IsIdle()) {
                return;
            }
        }

        if (auxEngaged_ && !auxIdle_) {
            return;
        }

        if (acceptorStarted_) {
            if (!acceptorDisposeRequested_) {
                acceptorDisposeRequested_ = true;
                acceptor_->dispose();
                return;
            }
            if (acceptorDisposedCount_ == 0) {
                return;
            }
        }

        if (singleSlot_.started) {
            if (!singleSlotDisposeRequested_) {
                singleSlotDisposeRequested_ = true;
                singleSlot_.Dispose();
                return;
            }
            if (singleSlot_.disposed == 0) {
                return;
            }
        }

        // Stop through a zero-delay timer instead of shutting down in place:
        // RequestFinish() may run before reactor.run() even starts, and a
        // direct shutdown() would skip the loop pass that drains the pending
        // dispose operations (terminal callbacks would never fire).
        if (stopTimer_ && stopTimer_->is_linked()) {
            return;
        }

        stopTimer_.emplace(timer_callback_t::create<TCPConnectionIOCPTest,
            &TCPConnectionIOCPTest::OnStop>(*this));
        stopTimer_->arm(clock_t::now());
        reactor_.addTimer(*stopTimer_);
    }

    void OnActionTimer() noexcept
    {
        if (actionDispose_) {
            actionDispose_ = false;
            driver_->dispose();
        }
    }

    void OnTimeout() noexcept
    {
        timedOut_ = true;
        finished_ = true;
        reactor_.shutdown();
    }

    void OnStop() noexcept
    {
        finished_ = true;
        reactor_.shutdown();
    }

    C_Reactor reactor_{};
    std::optional<Driver> driver_;
    std::optional<C_Timer> timeoutTimer_;
    std::optional<C_Timer> actionTimer_;
    std::optional<C_Timer> stopTimer_;

    etl::pool<Acceptor::accept_operation_t, 4> backlog_{};
    std::optional<Acceptor> acceptor_;
    std::optional<C_Address> acceptorAddress_;
    std::optional<C_Address> rebornAddress_;
    uint16_t acceptorPort_{0};
    SecondClientDelegate second_{*this};
    SingleSlotDelegate singleSlot_{*this};
    std::array<PeerDelegate, 4> peers_;

    const C_Address* secondConnectAddress_{nullptr};
    const C_Address* reconnectAddress_{nullptr};

    int readyReadCount_{0};
    int connectCount_{0};
    int disconnectCount_{0};
    int disposedCount_{0};
    int commitCount_{0};
    int acceptedCount_{0};
    int acceptorDisposedCount_{0};
    int acceptorErrorCount_{0};
    int loopbackRemoteCount_{0};
    int32_t acceptorLastReason_{0};
    int32_t acceptorLastError_{0};
    int32_t lastConnectError_{0};
    int32_t lastDisconnectError_{0};
    int32_t lastCommitError_{0};
    int32_t secondConnectError_{0};
    int32_t reconnectError_{0};
    int32_t sendOnConnectError_{0};
    int32_t chainedSendError_{0};
    C_Reactor::operation_t* lastCommitOperation_{nullptr};
    send_operation_t* sendOnConnect_{nullptr};
    send_operation_t* sendOnConnect2_{nullptr};
    send_operation_t* sendOnCommit_{nullptr};

    bool timedOut_{false};
    bool secondConnectAttempted_{false};
    bool secondConnectOk_{false};
    bool reconnectAttempted_{false};
    bool reconnectOk_{false};
    bool actionDispose_{false};
    bool sendOnConnectAttempted_{false};
    bool sendOnConnectOk_{false};
    bool secondSendOnConnectAttempted_{false};
    bool secondSendOnConnectOk_{false};
    bool chainedSendAttempted_{false};
    bool chainedSendOk_{false};

    bool consumeReadyRead_{false};
    bool captureReadyReadPayload_{false};
    bool disposeOnPeerCapture_{false};
    bool reconnectOnConnectError_{false};
    bool reconnectOnDisposed_{false};
    bool reconnectOnDisconnect_{false};
    bool disposeOnConnect_{false};
    bool disposeOnReadyRead_{false};
    bool disposeOnCommit_{false};
    int disposeAfterReadyReads_{0};
    int disposeAfterConnects_{0};
    int disposeAfterCommits_{0};
    int scheduleDisposeOnConnectMs_{-1};
    char capturedPayload_[64]{};
    int capturedPayloadLen_{0};

    bool finishRequested_{false};
    bool finished_{false};
    bool auxEngaged_{false};
    bool auxIdle_{false};
    bool acceptorStarted_{false};
    bool acceptorDisposeRequested_{false};
    bool disposeFromOnIncoming_{false};
    bool singleSlotDisposeRequested_{false};
    bool reuseAcceptorOnDisposed_{false};
    bool rebornInitOk_{false};
    bool rebornListenOk_{false};
    bool rebornConnectOk_{false};

    friend class PeerDelegate;
    friend class SecondClientDelegate;
};

// ---- PeerDelegate ----------------------------------------------------------

void PeerDelegate::Adopt(etsl::C_Socket fd) noexcept
{
    adoptedCount++;

    // Abortive mode keeps the accepted fd raw: the library's C_Socket::dispose()
    // always performs a graceful shutdown(BOTH) first, so an RST (the point of
    // this scenario) is only reachable through a direct closesocket with
    // SO_LINGER{1,0} set.
    if (cfg.abortiveClose) {
        linger ling{};
        ling.l_onoff = 1;
        ling.l_linger = 0;
        rawSocket_ = fd.release();
        (void)setsockopt(rawSocket_, SOL_SOCKET, SO_LINGER,
            reinterpret_cast<const char*>(&ling), sizeof(ling));

        if (cfg.closeOnAdopt) {
            Abort();
        }
        return;
    }

    conn_.emplace(owner_.reactor(), *this);
    if (!conn_->adopt(std::move(fd)).has_value()) {
        conn_.reset();
        return;
    }

    if (cfg.sendOnAdopt != nullptr) {
        Send(cfg.sendOnAdopt, cfg.sendOnAdoptLen);
    }

    if (cfg.closeOnAdopt) {
        Close();
    }
}

void PeerDelegate::Send(const uint8_t* data, const size_t len) noexcept
{
    if (!conn_.has_value()) {
        return;
    }

    sendOp_.content = etl::span<const uint8_t>{data, len};
    sendOp_.transferred = 0;
    sendOk = conn_->send(sendOp_.content, sendOp_).has_value();
}

void PeerDelegate::Close() noexcept
{
    if (conn_.has_value()) {
        conn_->dispose();
    }
}

void PeerDelegate::Abort() noexcept
{
    if (rawSocket_ != INVALID_SOCKET) {
        ::closesocket(rawSocket_);
        rawSocket_ = INVALID_SOCKET;
        aborted_ = true;
        owner_.NotifyPeerIdle();
    }
}

void PeerDelegate::Release() noexcept
{
    if (rawSocket_ != INVALID_SOCKET) {
        ::closesocket(rawSocket_);
        rawSocket_ = INVALID_SOCKET;
    }
    conn_.reset();
}

void PeerDelegate::onConnect(const int32_t /*error*/) noexcept
{
    // adopt() never reports onConnect for an already-connected socket (ADR-3);
    // the first peer event must be onReadyRead or onDisconnect.
    ADD_FAILURE() << "peer connection must not receive onConnect";
}

void PeerDelegate::onReadyRead() noexcept
{
    readyReadCount++;

    while (conn_.has_value()) {
        uint8_t chunk[64];
        // value_or(0): WOULDBLOCK ("no more data") and EOF/IO-error (teardown
        // started by the connection itself) both stop the drain loop.
        const uint32_t n = conn_->read(etl::span<uint8_t>{chunk, sizeof(chunk)}).value_or(0);
        if (n == 0) {
            break;
        }

        char* target = (cfg.captureBuf != nullptr) ? cfg.captureBuf : (cfg.tagEcho ? tag : nullptr);
        const int cap = (cfg.captureBuf != nullptr) ? cfg.captureCap : static_cast<int>(sizeof(tag));
        if (target != nullptr && capturedLen < cap) {
            const int room = cap - capturedLen;
            const auto copy = (n < static_cast<uint32_t>(room)) ? static_cast<int>(n) : room;
            std::memcpy(target + capturedLen, chunk, static_cast<size_t>(copy));
            capturedLen += copy;
        }
    }

    if (payloadNotified_) {
        return;
    }

    if (cfg.tagEcho && capturedLen >= 4) {
        payloadNotified_ = true;
        owner_.EchoByTag(*this);
    } else if (cfg.captureReport && cfg.captureCap > 0 && capturedLen >= cfg.captureCap) {
        payloadNotified_ = true;
        owner_.OnPeerCaptured(*this);
    }
}

void PeerDelegate::onCommit(etsl::C_Reactor::operation_t& /*operation*/, const int32_t error) noexcept
{
    commitCount++;
    lastCommitError = error;

    if (cfg.closeOnCommit && error == 0) {
        Close();
    }
}

void PeerDelegate::onDisconnect(const int32_t error) noexcept
{
    disconnectedCount++;
    lastDisconnectError = error;
    owner_.NotifyPeerIdle();
}

void PeerDelegate::onDisposed() noexcept
{
    disposedCount++;
    owner_.NotifyPeerIdle();
}

// ---- SecondClientDelegate ---------------------------------------------------

void SecondClientDelegate::Start(const etsl::C_Address& addr) noexcept
{
    conn_.emplace(owner_.reactor(), *this);
    startedOk_ = conn_->connect(addr).has_value();
}

void SecondClientDelegate::onConnect(const int32_t error) noexcept
{
    connectCount_++;
    lastConnectError_ = error;

    if (error == 0) {
        sendOp_.content = etl::span<const uint8_t>{sendBytes_, sizeof(sendBytes_)};
        sendOp_.transferred = 0;
        sendOk_ = conn_->send(sendOp_.content, sendOp_).has_value();
    }
}

void SecondClientDelegate::onReadyRead() noexcept
{
    readyReadCount_++;

    while (payloadLen_ < static_cast<int>(sizeof(payload_))) {
        // value_or(0): operator* is broken under -fno-exceptions; unexpected
        // (WOULDBLOCK drained / EOF / IO error — the latter two start teardown) → 0 → stop.
        const uint32_t n = conn_->read({
            reinterpret_cast<uint8_t*>(payload_ + payloadLen_),
            sizeof(payload_) - payloadLen_
        }).value_or(0);
        if (n == 0) {
            break;
        }
        payloadLen_ += static_cast<int>(n);
    }

    if (payloadLen_ >= 5) {
        conn_->dispose();
    }
}

void SecondClientDelegate::onCommit(etsl::C_Reactor::operation_t& /*operation*/, const int32_t error) noexcept
{
    commitCount_++;
    lastCommitError_ = error;
}

void SecondClientDelegate::onDisconnect(const int32_t error) noexcept
{
    disconnectCount_++;
    lastDisconnectError_ = error;
}

void SecondClientDelegate::onDisposed() noexcept
{
    disposedCount_++;
    owner_.NotifyAuxIdle();
}

// ---- SingleSlotDelegate ------------------------------------------------------

bool SingleSlotDelegate::Start(const etsl::C_Address& addr) noexcept
{
    acceptor_.emplace(owner_.reactor(), backlog, *this);
    if (!acceptor_->initialize(addr).has_value()) {
        return false;
    }

    if (!acceptor_->listen(4).has_value()) {
        return false;
    }

    started = true;
    return true;
}

void SingleSlotDelegate::Dispose() noexcept
{
    if (acceptor_.has_value()) {
        acceptor_->dispose();
    }
}

void SingleSlotDelegate::Release() noexcept
{
    held.dispose();
    acceptor_.reset();
}

void SingleSlotDelegate::onIncoming(etsl::C_Socket fd, const etsl::C_Address& /*remoteAddr*/) noexcept
{
    accepted++;
    // Keep the accepted session open as a raw socket; the pool slot rearms
    // itself in the completion tail either way.
    held = std::move(fd);
}

void SingleSlotDelegate::onError(const int32_t error) noexcept
{
    errors++;
    lastError = error;
}

void SingleSlotDelegate::onDisposed(const int32_t reason) noexcept
{
    disposed++;
    lastReason = reason;
    owner_.NotifySecondaryDisposed();
}

// ---- connection: dispose contract --------------------------------------------

TEST_F(TCPConnectionIOCPTest, DisposeWithoutConnectInvokesOnDisposed)
{
    driver_->dispose();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(connectCount_, 0);
    EXPECT_EQ(disconnectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, DoubleDisposeInvokesOnDisposedOnce)
{
    driver_->dispose();
    driver_->dispose();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
}

// ---- connection: connect and reconnects ---------------------------------------

TEST_F(TCPConnectionIOCPTest, ConnectToListeningPeerInvokesOnConnectSuccess)
{
    ASSERT_TRUE(StartAcceptor());

    disposeOnConnect_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    // NOTE: no accept assertions here. With dispose-on-connect the AcceptEx
    // completion races with the dispose cascade: the accept may dispatch
    // after the client already tore the session down (or be cancelled along
    // with the gateway). Deterministic accept coverage lives in the PeerData*
    // and TwoDrivers scenarios, where the session stays open.
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, SecondConnectWhileConnectingReturnsAlready)
{
    ASSERT_TRUE(StartAcceptor());

    disposeOnConnect_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());

    const auto second = driver_->connect(AcceptorAddress());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), static_cast<int32_t>(WSAEALREADY));

    RunReactor();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, SecondConnectWhileConnectedReturnsAlready)
{
    ASSERT_TRUE(StartAcceptor());

    const C_Address& addr = AcceptorAddress();
    secondConnectAddress_ = &addr;
    disposeOnConnect_ = true;

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_TRUE(secondConnectAttempted_);
    EXPECT_FALSE(secondConnectOk_);
    EXPECT_EQ(secondConnectError_, static_cast<int32_t>(WSAEALREADY));
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, ConnectWhileDisposingReturnsAlready)
{
    ASSERT_TRUE(StartAcceptor());

    const C_Address& addr = AcceptorAddress();

    ASSERT_TRUE(driver_->connect(addr).has_value());
    driver_->dispose();

    const auto again = driver_->connect(addr);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), static_cast<int32_t>(WSAEALREADY));

    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(connectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, ConnectRefusedInvokesOnConnectWithError)
{
    uint16_t deadPort = 0;
    ASSERT_TRUE(ReservePort(deadPort));

    C_Address addr;
    ASSERT_TRUE(MakeAddress(deadPort, addr));

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_NE(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 0);
    EXPECT_EQ(disconnectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, ReconnectAfterConnectFailureSucceeds)
{
    uint16_t deadPort = 0;
    ASSERT_TRUE(ReservePort(deadPort));

    C_Address deadAddr;
    ASSERT_TRUE(MakeAddress(deadPort, deadAddr));

    ASSERT_TRUE(StartAcceptor());

    reconnectAddress_ = &AcceptorAddress();
    reconnectOnConnectError_ = true;
    disposeOnConnect_ = true;

    ASSERT_TRUE(driver_->connect(deadAddr).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(reconnectAttempted_);
    EXPECT_TRUE(reconnectOk_);
    ASSERT_EQ(connectCount_, 2);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, ReconnectAfterDisposeSucceeds)
{
    ASSERT_TRUE(StartAcceptor());

    reconnectAddress_ = &AcceptorAddress();
    reconnectOnDisposed_ = true;
    disposeOnConnect_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(reconnectAttempted_);
    EXPECT_TRUE(reconnectOk_);
    EXPECT_EQ(connectCount_, 2);
    EXPECT_EQ(disposedCount_, 2);
}

TEST_F(TCPConnectionIOCPTest, ReconnectFromOnDisconnectSucceeds)
{
    ASSERT_TRUE(StartAcceptor());

    // The first peer instance closes the session as soon as it is accepted;
    // the client observes EOF, reconnects from onDisconnect and the second
    // peer instance holds the fresh session.
    peer(0).cfg.closeOnAdopt = true;

    consumeReadyRead_ = true;
    reconnectAddress_ = &AcceptorAddress();
    reconnectOnDisconnect_ = true;
    disposeAfterConnects_ = 2;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(reconnectAttempted_);
    EXPECT_TRUE(reconnectOk_);
    ASSERT_EQ(connectCount_, 2);
    EXPECT_EQ(lastConnectError_, 0);
    ASSERT_EQ(disconnectCount_, 1);
    EXPECT_EQ(lastDisconnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, DisposeDuringConnectInvokesOnDisposed)
{
    ASSERT_TRUE(StartAcceptor());

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    driver_->dispose();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);
    EXPECT_EQ(connectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, DisposeWhileConnectedInvokesOnDisposed)
{
    ASSERT_TRUE(StartAcceptor());

    scheduleDisposeOnConnectMs_ = 10;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);
}

// ---- connection: read-probe ----------------------------------------------------

TEST_F(TCPConnectionIOCPTest, PeerDataInvokesOnReadyRead)
{
    ASSERT_TRUE(StartAcceptor());

    static constexpr uint8_t kPayload[] = "ping";
    peer(0).cfg.sendOnAdopt = kPayload;
    peer(0).cfg.sendOnAdoptLen = sizeof(kPayload) - 1;
    disposeOnReadyRead_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(peer(0).adoptedCount, 1);
    EXPECT_TRUE(peer(0).sendOk);
    // onIncoming must deliver the resolved remote endpoint of the accepted
    // socket: the loopback client is always 127.0.0.1.
    EXPECT_EQ(loopbackRemoteCount_, 1);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_GE(readyReadCount_, 1);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, PeerDataReadableExactPayload)
{
    ASSERT_TRUE(StartAcceptor());

    static constexpr char kPayload[] = "hello-etsl";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);

    static constexpr uint8_t kBytes[] = "hello-etsl";
    peer(0).cfg.sendOnAdopt = kBytes;
    peer(0).cfg.sendOnAdoptLen = sizeof(kBytes) - 1;
    captureReadyReadPayload_ = true;
    disposeOnReadyRead_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(peer(0).adoptedCount, 1);
    EXPECT_TRUE(peer(0).sendOk);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, kPayloadLen);
    EXPECT_EQ(std::memcmp(capturedPayload_, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, PeerSendsWhileConnectInProgressIsDelivered)
{
    ASSERT_TRUE(StartAcceptor());

    static constexpr char kPayload[] = "early";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);

    // The peer sends immediately on adopt, i.e. before the client's ConnectEx
    // completion is dispatched and its read probe is armed: the payload must
    // be buffered by the stack and delivered once the probe comes up.
    static constexpr uint8_t kBytes[] = "early";
    peer(0).cfg.sendOnAdopt = kBytes;
    peer(0).cfg.sendOnAdoptLen = sizeof(kBytes) - 1;
    captureReadyReadPayload_ = true;
    disposeOnReadyRead_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(peer(0).adoptedCount, 1);
    EXPECT_TRUE(peer(0).sendOk);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    ASSERT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, kPayloadLen);
    EXPECT_EQ(std::memcmp(capturedPayload_, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, ReadyReadRearmsWhileDataUnread)
{
    ASSERT_TRUE(StartAcceptor());

    static constexpr uint8_t kPayload[] = "ping";
    peer(0).cfg.sendOnAdopt = kPayload;
    peer(0).cfg.sendOnAdoptLen = sizeof(kPayload) - 1;
    disposeAfterReadyReads_ = 2;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_GE(readyReadCount_, 2);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, PeerGracefulCloseInvokesOnDisconnect)
{
    ASSERT_TRUE(StartAcceptor());

    peer(0).cfg.closeOnAdopt = true;
    // EOF is observed only via driver.read(); that starts teardown → onDisconnect(0).
    consumeReadyRead_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    ASSERT_EQ(disconnectCount_, 1);
    EXPECT_EQ(lastDisconnectError_, 0);
    EXPECT_EQ(disposedCount_, 0);
    EXPECT_EQ(peer(0).disposedCount, 1);
}

TEST_F(TCPConnectionIOCPTest, PeerAbortiveCloseInvokesOnDisconnectWithError)
{
    ASSERT_TRUE(StartAcceptor());

    peer(0).cfg.abortiveClose = true;
    peer(0).cfg.closeOnAdopt = true;
    consumeReadyRead_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    ASSERT_EQ(disconnectCount_, 1);
    EXPECT_NE(lastDisconnectError_, 0);
    EXPECT_EQ(disposedCount_, 0);
    EXPECT_TRUE(peer(0).IsAborted() || peer(0).disposedCount == 1);
}

TEST_F(TCPConnectionIOCPTest, PeerDataThenGracefulClose)
{
    ASSERT_TRUE(StartAcceptor());

    // Data is followed by a graceful close on the same commit: TCP orders the
    // payload before the FIN, so the client reads "bye" and then EOF.
    static constexpr uint8_t kBytes[] = "bye";
    peer(0).cfg.sendOnAdopt = kBytes;
    peer(0).cfg.sendOnAdoptLen = sizeof(kBytes) - 1;
    peer(0).cfg.closeOnCommit = true;
    consumeReadyRead_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_GE(readyReadCount_, 1);
    ASSERT_EQ(disconnectCount_, 1);
    EXPECT_EQ(lastDisconnectError_, 0);
    EXPECT_EQ(disposedCount_, 0);
    EXPECT_EQ(peer(0).disposedCount, 1);
    EXPECT_EQ(peer(0).lastCommitError, 0);
}

TEST_F(TCPConnectionIOCPTest, ExplicitDisposeAfterReadyReadDoesNotEmitDisconnect)
{
    ASSERT_TRUE(StartAcceptor());

    static constexpr uint8_t kBytes[] = "x";
    peer(0).cfg.sendOnAdopt = kBytes;
    peer(0).cfg.sendOnAdoptLen = sizeof(kBytes) - 1;
    disposeOnReadyRead_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_GE(readyReadCount_, 1);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, ReadBeforeConnectReturnsNotConn)
{
    uint8_t bytes[8]{};
    const auto result = driver_->read({bytes, sizeof(bytes)});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAENOTCONN));
}

TEST_F(TCPConnectionIOCPTest, SendBeforeConnectReturnsNotConn)
{
    uint8_t bytes[] = {'x'};
    send_operation_t op{};
    PrepareSend(op, bytes, sizeof(bytes));

    const auto result = driver_->send(op.content, op);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAENOTCONN));
    EXPECT_EQ(commitCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, SendAfterDisconnectReturnsNotConn)
{
    ASSERT_TRUE(StartAcceptor());

    peer(0).cfg.closeOnAdopt = true;
    consumeReadyRead_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    ASSERT_EQ(disconnectCount_, 1);

    uint8_t bytes[] = {'x'};
    send_operation_t op{};
    PrepareSend(op, bytes, sizeof(bytes));

    const auto result = driver_->send(op.content, op);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAENOTCONN));
    EXPECT_EQ(commitCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, SendAfterDisposeReturnsNotConn)
{
    driver_->dispose();
    RunReactor();
    ASSERT_EQ(disposedCount_, 1);

    uint8_t bytes[] = {'x'};
    send_operation_t op{};
    PrepareSend(op, bytes, sizeof(bytes));

    const auto result = driver_->send(op.content, op);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAENOTCONN));
    EXPECT_EQ(commitCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, SendEmptyWhileConnectedReturnsInvalid)
{
    ASSERT_TRUE(StartAcceptor());

    send_operation_t emptyOp{};
    sendOnConnect_ = &emptyOp;
    disposeOnConnect_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sendOnConnectAttempted_);
    EXPECT_FALSE(sendOnConnectOk_);
    EXPECT_EQ(sendOnConnectError_, static_cast<int32_t>(WSAEINVAL));
    EXPECT_EQ(commitCount_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

// ---- connection: adopt ---------------------------------------------------------

TEST_F(TCPConnectionIOCPTest, AdoptConnectedSocketReceivesPeerData)
{
    ASSERT_TRUE(StartAcceptor());

    C_Socket fd;
    ASSERT_TRUE(MakeConnectedSocket(AcceptorPort(), fd));

    const auto adopted = driver_->adopt(std::move(fd));
    ASSERT_TRUE(adopted.has_value()) << "adopt error: " << adopted.error();

    static constexpr char kPayload[] = "adopted";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);

    static constexpr uint8_t kBytes[] = "adopted";
    peer(0).cfg.sendOnAdopt = kBytes;
    peer(0).cfg.sendOnAdoptLen = sizeof(kBytes) - 1;
    captureReadyReadPayload_ = true;
    disposeOnReadyRead_ = true;

    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(peer(0).adoptedCount, 1);
    EXPECT_TRUE(peer(0).sendOk);
    ASSERT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, kPayloadLen);
    EXPECT_EQ(std::memcmp(capturedPayload_, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, AdoptedSocketSendsAndPeerReceives)
{
    ASSERT_TRUE(StartAcceptor());

    C_Socket fd;
    ASSERT_TRUE(MakeConnectedSocket(AcceptorPort(), fd));

    const auto adopted = driver_->adopt(std::move(fd));
    ASSERT_TRUE(adopted.has_value()) << "adopt error: " << adopted.error();

    static constexpr char kPayload[] = "pushed";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);
    uint8_t bytes[kPayloadLen];
    std::memcpy(bytes, kPayload, sizeof(bytes));

    // Adopted connection is CONNECTED synchronously: send right away, no onConnect.
    send_operation_t op{};
    PrepareSend(op, bytes, sizeof(bytes));
    const auto sent = driver_->send(op.content, op);
    ASSERT_TRUE(sent.has_value()) << "send error: " << sent.error();

    char received[kPayloadLen]{};
    peer(0).cfg.captureBuf = received;
    peer(0).cfg.captureCap = kPayloadLen;
    peer(0).cfg.captureReport = true;
    disposeOnPeerCapture_ = true;

    RunReactor();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(peer(0).capturedLen, kPayloadLen);
    EXPECT_EQ(std::memcmp(received, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    ASSERT_EQ(commitCount_, 1);
    EXPECT_EQ(lastCommitError_, 0);
    EXPECT_EQ(op.transferred, static_cast<uint32_t>(kPayloadLen));
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, AdoptWhileConnectedReturnsIsConn)
{
    ASSERT_TRUE(StartAcceptor());

    C_Socket fd1;
    C_Socket fd2;
    ASSERT_TRUE(MakeConnectedSocket(AcceptorPort(), fd1));
    ASSERT_TRUE(MakeConnectedSocket(AcceptorPort(), fd2));

    ASSERT_TRUE(driver_->adopt(std::move(fd1)).has_value());

    const auto again = driver_->adopt(std::move(fd2));
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), static_cast<int32_t>(WSAEISCONN));

    // The second raw socket is never adopted; drop it now so the corresponding
    // peer slot observes the reset and drains before the loop stops.
    fd2.dispose();

    driver_->dispose();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
}

// WOULDBLOCK is an explicit "no more data" answer: not a success value, but
// also not a teardown — the connection must stay fully operational afterwards.
TEST_F(TCPConnectionIOCPTest, ReadWithoutDataReturnsWouldBlockConnectionStaysAlive)
{
    ASSERT_TRUE(StartAcceptor());

    C_Socket fd;
    ASSERT_TRUE(MakeConnectedSocket(AcceptorPort(), fd));

    const auto adopted = driver_->adopt(std::move(fd));
    ASSERT_TRUE(adopted.has_value());

    // Nothing was sent by the peer yet: read must report WOULDBLOCK and keep
    // the connection alive (no teardown, no onDisconnect).
    uint8_t buf[8]{};
    const auto empty = driver_->read({buf, sizeof(buf)});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error(), static_cast<int32_t>(WSAEWOULDBLOCK));
    EXPECT_EQ(disconnectCount_, 0);
    EXPECT_EQ(disposedCount_, 0);

    // The connection is still operational: peer data is delivered and readable.
    static constexpr char kPayload[] = "alive";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);

    static constexpr uint8_t kBytes[] = "alive";
    peer(0).cfg.sendOnAdopt = kBytes;
    peer(0).cfg.sendOnAdoptLen = sizeof(kBytes) - 1;
    captureReadyReadPayload_ = true;
    disposeOnReadyRead_ = true;

    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(peer(0).adoptedCount, 1);
    EXPECT_TRUE(peer(0).sendOk);
    ASSERT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, kPayloadLen);
    EXPECT_EQ(std::memcmp(capturedPayload_, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);
}

// ---- connection: proactive send -------------------------------------------------

TEST_F(TCPConnectionIOCPTest, SendAfterConnectInvokesOnCommitAndDeliversPayload)
{
    ASSERT_TRUE(StartAcceptor());

    static constexpr char kPayload[] = "send-payload";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);
    uint8_t bytes[sizeof(kPayload) - 1];
    std::memcpy(bytes, kPayload, static_cast<size_t>(kPayloadLen));

    send_operation_t op{};
    PrepareSend(op, bytes, static_cast<size_t>(kPayloadLen));
    sendOnConnect_ = &op;

    char received[sizeof(kPayload)]{};
    peer(0).cfg.captureBuf = received;
    peer(0).cfg.captureCap = kPayloadLen;
    peer(0).cfg.captureReport = true;
    disposeOnPeerCapture_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sendOnConnectAttempted_);
    EXPECT_TRUE(sendOnConnectOk_);
    ASSERT_EQ(peer(0).capturedLen, kPayloadLen);
    EXPECT_EQ(std::memcmp(received, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    ASSERT_EQ(commitCount_, 1);
    EXPECT_EQ(lastCommitError_, 0);
    EXPECT_EQ(lastCommitOperation_, static_cast<C_Reactor::operation_t*>(&op));
    EXPECT_EQ(op.transferred, static_cast<uint32_t>(kPayloadLen));
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, SequentialSendsFromOnCommit)
{
    ASSERT_TRUE(StartAcceptor());

    uint8_t firstBytes[] = {'A', 'A', 'A', 'A'};
    uint8_t secondBytes[] = {'B', 'B', 'B', 'B'};
    send_operation_t first{};
    send_operation_t second{};
    PrepareSend(first, firstBytes, sizeof(firstBytes));
    PrepareSend(second, secondBytes, sizeof(secondBytes));

    sendOnConnect_ = &first;
    sendOnCommit_ = &second;

    char received[8]{};
    peer(0).cfg.captureBuf = received;
    peer(0).cfg.captureCap = 8;
    peer(0).cfg.captureReport = true;
    disposeOnPeerCapture_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sendOnConnectOk_);
    EXPECT_TRUE(chainedSendAttempted_);
    EXPECT_TRUE(chainedSendOk_);
    ASSERT_EQ(peer(0).capturedLen, 8);
    EXPECT_EQ(std::memcmp(received, "AAAABBBB", 8), 0);
    EXPECT_EQ(commitCount_, 2);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, ConcurrentSendOperationsAreDeliveredInOrder)
{
    ASSERT_TRUE(StartAcceptor());

    // Each send carries its own send_operation_t context; both are posted from onConnect
    // without waiting for the first commit. TCP preserves the posting order on the wire.
    static constexpr char kFirst[] = "AAAA";
    static constexpr char kSecond[] = "BBBB";

    uint8_t firstBytes[sizeof(kFirst) - 1];
    uint8_t secondBytes[sizeof(kSecond) - 1];
    std::memcpy(firstBytes, kFirst, sizeof(firstBytes));
    std::memcpy(secondBytes, kSecond, sizeof(secondBytes));

    send_operation_t first{};
    send_operation_t second{};
    PrepareSend(first, firstBytes, sizeof(firstBytes));
    PrepareSend(second, secondBytes, sizeof(secondBytes));

    sendOnConnect_ = &first;
    sendOnConnect2_ = &second;

    char received[8]{};
    peer(0).cfg.captureBuf = received;
    peer(0).cfg.captureCap = 8;
    peer(0).cfg.captureReport = true;
    disposeOnPeerCapture_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sendOnConnectOk_);
    EXPECT_TRUE(secondSendOnConnectOk_);
    ASSERT_EQ(peer(0).capturedLen, 8);
    EXPECT_EQ(std::memcmp(received, "AAAABBBB", 8), 0);
    EXPECT_EQ(commitCount_, 2);
    EXPECT_EQ(lastCommitError_, 0);
    EXPECT_EQ(first.transferred, static_cast<uint32_t>(sizeof(firstBytes)));
    EXPECT_EQ(second.transferred, static_cast<uint32_t>(sizeof(secondBytes)));
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, DisposeDuringPendingSendInvokesOnDisposed)
{
    ASSERT_TRUE(StartAcceptor());

    std::vector<uint8_t> bytes(64 * 1024, static_cast<uint8_t>('Z'));
    send_operation_t op{};
    PrepareSend(op, bytes.data(), bytes.size());
    sendOnConnect_ = &op;
    disposeOnConnect_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sendOnConnectAttempted_);
    EXPECT_TRUE(sendOnConnectOk_);
    EXPECT_EQ(disposedCount_, 1);
    // Cancelled/aborted send still delivers terminal onCommit so the user can free the op.
    ASSERT_GE(commitCount_, 1);
    EXPECT_NE(lastCommitError_, 0);
    EXPECT_EQ(disconnectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, TwoDriversOnSameReactorOperateIndependently)
{
    ASSERT_TRUE(StartAcceptor());

    const C_Address& addr = AcceptorAddress();

    // Equal-length connect-time tags let the peers identify connections by
    // content instead of accept order (accept order races with ConnectEx);
    // each peer slot echoes a distinct payload per tag.
    for (auto& slot : peers_) {
        slot.cfg.tagEcho = true;
    }

    static constexpr char kFirstTag[] = "FIX1";
    uint8_t firstBytes[sizeof(kFirstTag) - 1];
    std::memcpy(firstBytes, kFirstTag, sizeof(firstBytes));

    send_operation_t first{};
    PrepareSend(first, firstBytes, sizeof(firstBytes));
    sendOnConnect_ = &first;
    captureReadyReadPayload_ = true;
    disposeAfterReadyReads_ = 1;

    StartSecondClient(addr);

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(acceptedCount_, 2);
    EXPECT_EQ(loopbackRemoteCount_, 2);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    ASSERT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, 5);
    EXPECT_EQ(std::memcmp(capturedPayload_, "echoA", 5), 0);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);

    EXPECT_TRUE(second_.startedOk_);
    ASSERT_EQ(second_.connectCount_, 1);
    EXPECT_EQ(second_.lastConnectError_, 0);
    EXPECT_TRUE(second_.sendOk_);
    ASSERT_EQ(second_.payloadLen_, 5);
    EXPECT_EQ(std::memcmp(second_.payload_, "echoB", 5), 0);
    EXPECT_EQ(second_.lastCommitError_, 0);
    ASSERT_EQ(second_.disposedCount_, 1);
    EXPECT_EQ(second_.disconnectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, SendWhileConnectingReturnsNotConn)
{
    ASSERT_TRUE(StartAcceptor());

    disposeOnConnect_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());

    uint8_t bytes[] = {'x'};
    send_operation_t op{};
    PrepareSend(op, bytes, sizeof(bytes));
    const auto result = driver_->send(op.content, op);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAENOTCONN));

    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(commitCount_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

// ---- acceptor contract ----------------------------------------------------------

TEST_F(TCPConnectionIOCPTest, AcceptorListenWithoutInitializeReturnsInvalid)
{
    EmplaceAcceptor();

    const auto result = acceptor_->listen(4);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAEINVAL));

    acceptor_->dispose();
    RequestFinish();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(acceptorDisposedCount_, 1);
    // The EXPLICIT_DISPOSE reason (acceptor contract) must reach onDisposed.
    EXPECT_EQ(acceptorLastReason_, -1);
}

TEST_F(TCPConnectionIOCPTest, AcceptorDisposeBeforeListenInvokesOnDisposedWithExplicitReason)
{
    EmplaceAcceptor();

    acceptor_->dispose();
    RequestFinish();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(acceptorDisposedCount_, 1);
    EXPECT_EQ(acceptorLastReason_, -1);
    EXPECT_EQ(acceptedCount_, 0);
    // Nothing was ever armed: no error reports, no accept reports.
    EXPECT_EQ(acceptorErrorCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, AcceptorListenTwiceReturnsInvalid)
{
    ASSERT_TRUE(StartAcceptor());

    const auto again = acceptor_->listen(4);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), static_cast<int32_t>(WSAEINVAL));

    RequestFinish();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(acceptorDisposedCount_, 1);
    EXPECT_EQ(acceptorLastReason_, -1);
    // The pool holds 4 armed AcceptEx operations; closing the gateway aborts
    // every one of them and the acceptor drains the slots silently — the
    // OPERATION_ABORTED path never reaches onError.
    EXPECT_EQ(acceptorErrorCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, AcceptorInitializeTwiceReturnsAlready)
{
    EmplaceAcceptor();

    uint16_t port = 0;
    ASSERT_TRUE(ReservePort(port));

    C_Address addr;
    ASSERT_TRUE(MakeAddress(port, addr));

    ASSERT_TRUE(acceptor_->initialize(addr).has_value());

    // The gateway is already bound while the state is NONE and the backlog
    // pool is still empty: the second initialize must be rejected by the
    // state/gateway guard with WSAEALREADY, not by a bind failure.
    const auto second = acceptor_->initialize(addr);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), static_cast<int32_t>(WSAEALREADY));

    acceptor_->dispose();
    RequestFinish();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(acceptorDisposedCount_, 1);
    EXPECT_EQ(acceptorLastReason_, -1);
    EXPECT_EQ(acceptorErrorCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, AcceptorInitializeAfterListenReturnsAlready)
{
    ASSERT_TRUE(StartAcceptor());

    // The state/gateway guard is checked first, so re-initialization is
    // rejected with WSAEALREADY even though the backlog pool is occupied by
    // the armed AcceptEx operations.
    const auto second = acceptor_->initialize(AcceptorAddress());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), static_cast<int32_t>(WSAEALREADY));

    RequestFinish();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(acceptorDisposedCount_, 1);
    EXPECT_EQ(acceptorLastReason_, -1);
}

TEST_F(TCPConnectionIOCPTest, AcceptorInitializeWithForeignBacklogFails)
{
    // The backlog pool must carry accept_operation_t-sized items.
    etl::pool<uint32_t, 4> foreignBacklog{};
    Acceptor foreign(reactor_, foreignBacklog, *this);

    uint16_t port = 0;
    ASSERT_TRUE(ReservePort(port));

    C_Address addr;
    ASSERT_TRUE(MakeAddress(port, addr));

    const auto result = foreign.initialize(addr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAEINVAL));

    foreign.dispose();
    RequestFinish();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(acceptorDisposedCount_, 1);
    EXPECT_EQ(acceptorLastReason_, -1);
    EXPECT_EQ(acceptorErrorCount_, 0);
}

// Reentrant dispose from inside the accept delivery. The completion tail runs
// after the delegate has returned (state already DISPOSING): the slot rearm
// must fail silently, the armAcceptBacklog failure must not overwrite the
// EXPLICIT_DISPOSE reason, and the accepted connection — handed out just
// before the teardown — must stay fully operational on its own.
TEST_F(TCPConnectionIOCPTest, AcceptorDisposeFromOnIncomingDrainsBacklogAndKeepsSession)
{
    ASSERT_TRUE(StartAcceptor());

    static constexpr char kPayload[] = "ping";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);

    static constexpr uint8_t kBytes[] = "ping";
    peer(0).cfg.sendOnAdopt = kBytes;
    peer(0).cfg.sendOnAdoptLen = sizeof(kBytes) - 1;
    captureReadyReadPayload_ = true;
    disposeOnReadyRead_ = true;
    disposeFromOnIncoming_ = true;

    ASSERT_TRUE(driver_->connect(AcceptorAddress()).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    // The accept delivery is deterministic here: the client holds the session
    // open until the payload arrives, long past the accept completion.
    EXPECT_EQ(acceptedCount_, 1);
    EXPECT_TRUE(peer(0).sendOk);
    ASSERT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, kPayloadLen);
    EXPECT_EQ(std::memcmp(capturedPayload_, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    EXPECT_EQ(disposedCount_, 1);

    // Acceptor teardown contract: exactly one terminal onDisposed, the
    // EXPLICIT_DISPOSE reason preserved, aborted backlog slots (this one plus
    // the three still-pending) never reported to onError.
    EXPECT_EQ(acceptorDisposedCount_, 1);
    EXPECT_EQ(acceptorLastReason_, -1);
    EXPECT_EQ(acceptorErrorCount_, 0);

    // The accepted session outlived the acceptor and ended cleanly.
    EXPECT_TRUE(peer(0).IsIdle());
}

// A single-slot backlog pool: listen arms exactly one AcceptEx, the accept
// cycles through that single element (rearm after delivery), and the teardown
// drains the pool through the same lone operation — backlog empty at the
// terminal onDisposed, no errors reported.
TEST_F(TCPConnectionIOCPTest, AcceptorSingleSlotBacklogTeardownThroughLastOperation)
{
    uint16_t port = 0;
    ASSERT_TRUE(ReservePort(port));

    C_Address addr;
    ASSERT_TRUE(MakeAddress(port, addr));

    ASSERT_TRUE(singleSlot_.Start(addr));
    ASSERT_EQ(singleSlot_.backlog.size(), static_cast<size_t>(1));

    // Hold the client session past the accept delivery (timer dispose), so
    // the single slot deterministically accepts and rearms.
    scheduleDisposeOnConnectMs_ = 30;

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);

    // One accept through the single element; the rearmed operation was the
    // last one standing when the teardown began.
    EXPECT_EQ(singleSlot_.accepted, 1);
    EXPECT_TRUE(singleSlot_.held.is_valid());
    EXPECT_EQ(singleSlot_.disposed, 1);
    EXPECT_EQ(singleSlot_.lastReason, -1);
    EXPECT_EQ(singleSlot_.errors, 0);
    EXPECT_EQ(singleSlot_.backlog.size(), static_cast<size_t>(0));
}

// Object rebirth after the terminal onDisposed: initialize()/listen() called
// reentrantly from the callback must succeed — their guards double as the
// clean-state checks (state NONE, gateway closed, backlog drained, reason
// cache reset) — and the reborn acceptor must serve a full session.
TEST_F(TCPConnectionIOCPTest, AcceptorReuseAfterDisposedReinitializesCleanly)
{
    uint16_t port1 = 0;
    uint16_t port2 = 0;
    ASSERT_TRUE(ReservePort(port1));
    ASSERT_TRUE(ReservePort(port2));

    acceptorAddress_.emplace();
    ASSERT_TRUE(acceptorAddress_->initialize("127.0.0.1", port1).has_value());
    rebornAddress_.emplace();
    ASSERT_TRUE(rebornAddress_->initialize("127.0.0.1", port2).has_value());

    EmplaceAcceptor();
    ASSERT_TRUE(acceptor_->initialize(*acceptorAddress_).has_value());
    ASSERT_TRUE(acceptor_->listen(4).has_value());
    acceptorStarted_ = true;

    reuseAcceptorOnDisposed_ = true;

    // Session on the reborn acceptor: deterministic accept — the peer feeds
    // data and the client holds the session until the payload arrives.
    static constexpr char kPayload[] = "ping";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);

    static constexpr uint8_t kBytes[] = "ping";
    peer(0).cfg.sendOnAdopt = kBytes;
    peer(0).cfg.sendOnAdoptLen = sizeof(kBytes) - 1;
    captureReadyReadPayload_ = true;
    disposeOnReadyRead_ = true;

    // Lifecycle 1 ends before any client connects.
    acceptor_->dispose();
    RequestFinish();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    // Reentrant rebirth from onDisposed succeeded and served a full session.
    EXPECT_TRUE(rebornInitOk_);
    EXPECT_TRUE(rebornListenOk_);
    EXPECT_TRUE(rebornConnectOk_);
    ASSERT_EQ(acceptorDisposedCount_, 2);
    EXPECT_EQ(acceptedCount_, 1);
    ASSERT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, kPayloadLen);
    EXPECT_EQ(std::memcmp(capturedPayload_, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
    // Both lifecycles ended with a fresh EXPLICIT_DISPOSE reason; the fixture
    // pool drained completely after the final teardown.
    EXPECT_EQ(acceptorLastReason_, -1);
    EXPECT_EQ(acceptorErrorCount_, 0);
    EXPECT_EQ(backlog_.size(), static_cast<size_t>(0));
}

} // namespace
