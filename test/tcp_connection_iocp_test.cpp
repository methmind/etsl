#include <gtest/gtest.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <etl/chrono.h>
#include <etl/span.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

import etsl;

namespace {

class LocalListener
{
public:
    ~LocalListener() noexcept
    {
        Close();
    }

    [[nodiscard]] bool Start() noexcept
    {
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) {
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            Close();
            return false;
        }

        if (::listen(listen_, 8) == SOCKET_ERROR) {
            Close();
            return false;
        }

        int addrLen = sizeof(addr);
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR) {
            Close();
            return false;
        }

        port_ = ntohs(addr.sin_port);
        return port_ != 0;
    }

    [[nodiscard]] uint16_t Port() const noexcept { return port_; }

    [[nodiscard]] bool Accept(const DWORD timeoutMs = 2000) noexcept
    {
        if (listen_ == INVALID_SOCKET) {
            return false;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listen_, &readSet);

        timeval tv{};
        tv.tv_sec = static_cast<long>(timeoutMs / 1000);
        tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);

        if (::select(0, &readSet, nullptr, nullptr, &tv) <= 0) {
            return false;
        }

        CloseAccepted();
        accepted_ = ::accept(listen_, nullptr, nullptr);
        return accepted_ != INVALID_SOCKET;
    }

    [[nodiscard]] bool SendAll(const char* data, const int len) const noexcept
    {
        if (accepted_ == INVALID_SOCKET) {
            return false;
        }

        int sent = 0;
        while (sent < len) {
            const int n = ::send(accepted_, data + sent, len - sent, 0);
            if (n <= 0) {
                return false;
            }
            sent += n;
        }
        return true;
    }

    [[nodiscard]] bool RecvExact(char* data, const int len, const DWORD timeoutMs = 2000) const noexcept
    {
        if (accepted_ == INVALID_SOCKET) {
            return false;
        }

        int got = 0;
        while (got < len) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(accepted_, &readSet);

            timeval tv{};
            tv.tv_sec = static_cast<long>(timeoutMs / 1000);
            tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);

            if (::select(0, &readSet, nullptr, nullptr, &tv) <= 0) {
                return false;
            }

            const int n = ::recv(accepted_, data + got, len - got, 0);
            if (n <= 0) {
                return false;
            }
            got += n;
        }
        return true;
    }

    [[nodiscard]] SOCKET AcceptRaw(const DWORD timeoutMs = 2000) noexcept
    {
        if (listen_ == INVALID_SOCKET) {
            return INVALID_SOCKET;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listen_, &readSet);

        timeval tv{};
        tv.tv_sec = static_cast<long>(timeoutMs / 1000);
        tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);

        if (::select(0, &readSet, nullptr, nullptr, &tv) <= 0) {
            return INVALID_SOCKET;
        }

        return ::accept(listen_, nullptr, nullptr);
    }

    [[nodiscard]] bool SendAllOn(const SOCKET s, const char* data, const int len) const noexcept
    {
        int sent = 0;
        while (sent < len) {
            const int n = ::send(s, data + sent, len - sent, 0);
            if (n <= 0) {
                return false;
            }
            sent += n;
        }
        return true;
    }

    [[nodiscard]] bool RecvExactOn(const SOCKET s, char* data, const int len, const DWORD timeoutMs = 2000) const noexcept
    {
        int got = 0;
        while (got < len) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(s, &readSet);

            timeval tv{};
            tv.tv_sec = static_cast<long>(timeoutMs / 1000);
            tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);

            if (::select(0, &readSet, nullptr, nullptr, &tv) <= 0) {
                return false;
            }

            const int n = ::recv(s, data + got, len - got, 0);
            if (n <= 0) {
                return false;
            }
            got += n;
        }
        return true;
    }

    void CloseAccepted() noexcept
    {
        if (accepted_ == INVALID_SOCKET) {
            return;
        }

        ::shutdown(accepted_, SD_BOTH);
        ::closesocket(accepted_);
        accepted_ = INVALID_SOCKET;
    }

    void AbortAccepted() noexcept
    {
        if (accepted_ == INVALID_SOCKET) {
            return;
        }

        linger ling{};
        ling.l_onoff = 1;
        ling.l_linger = 0;
        ::setsockopt(accepted_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&ling), sizeof(ling));
        ::closesocket(accepted_);
        accepted_ = INVALID_SOCKET;
    }

    void Close() noexcept
    {
        CloseAccepted();
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET listen_{INVALID_SOCKET};
    SOCKET accepted_{INVALID_SOCKET};
    uint16_t port_{0};
};

class SecondPeerDelegate;

// Second driver instance on the same reactor; used to verify per-instance isolation.
// Does not reference the fixture type: it stops the shared reactor once both drivers
// report onDisposed (the fixture's disposed count is passed in as a raw pointer).
class SecondPeerDelegate
{
public:
    using C_Reactor = etsl::C_Reactor;
    using C_Address = etsl::C_Address;
    using send_operation_t = etsl::send_operation_t;
    using Driver = etsl::C_TCPConnection<SecondPeerDelegate>;

    SecondPeerDelegate(C_Reactor& reactor, const int* ownerDisposedCount) noexcept
        : reactor_(reactor), ownerDisposedCount_(ownerDisposedCount) {}

    void Start(const C_Address& addr) noexcept
    {
        driver_.emplace(reactor_, *this);
        startedOk_ = driver_->connect(addr).has_value();
    }

    void onConnect(const int32_t error) noexcept
    {
        connectCount_++;
        lastConnectError_ = error;

        if (error == 0) {
            sendOp_.content = etl::span<const uint8_t>{sendBytes_, sizeof(sendBytes_)};
            sendOp_.transferred = 0;
            sendOk_ = driver_->send(sendOp_.content, sendOp_).has_value();
        }
    }

    void onReadyRead() noexcept
    {
        readyReadCount_++;

        constexpr uint32_t kReadFailed = UINT32_MAX;
        while (payloadLen_ < static_cast<int>(sizeof(payload_))) {
            const uint32_t n = driver_->read({
                reinterpret_cast<uint8_t*>(payload_ + payloadLen_),
                sizeof(payload_) - payloadLen_
            }).value_or(kReadFailed);
            if (n == kReadFailed || n == 0) {
                break;
            }
            payloadLen_ += static_cast<int>(n);
        }

        driver_->dispose();
    }

    void onCommit(C_Reactor::operation_t&, const int32_t error) noexcept
    {
        commitCount_++;
        lastCommitError_ = error;
    }

    void onDisconnect(const int32_t error) noexcept
    {
        disconnectCount_++;
        lastDisconnectError_ = error;
    }

    void onDisposed() noexcept
    {
        disposedCount_++;
        if (ownerDisposedCount_ != nullptr && *ownerDisposedCount_ > 0) {
            reactor_.shutdown();
        }
    }

    std::optional<Driver> driver_;

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
    C_Reactor& reactor_;
    const int* ownerDisposedCount_{nullptr};
    send_operation_t sendOp_{};
    uint8_t sendBytes_[4]{'2', 'n', 'd', '!'};
};

class TCPConnectionIOCPTest : public ::testing::Test
{
public:
    using clock_t = etsl::clock_t;
    using timer_callback_t = etsl::timer_callback_t;
    using C_Address = etsl::C_Address;
    using C_Reactor = etsl::C_Reactor;
    using C_Timer = etsl::C_Timer;
    using send_operation_t = etsl::send_operation_t;
    using Driver = etsl::C_TCPConnection<TCPConnectionIOCPTest>;

    void onReadyRead() noexcept
    {
        readyReadCount_++;

        if (captureReadyReadPayload_) {
            capturedPayloadLen_ = 0;
            while (capturedPayloadLen_ < static_cast<int>(sizeof(capturedPayload_))) {
                // value_or: avoid etl::expected<uint32_t>::operator* (broken under -fno-exceptions).
                constexpr uint32_t kReadFailed = UINT32_MAX;
                const uint32_t n = driver_->read({
                    reinterpret_cast<uint8_t*>(capturedPayload_ + capturedPayloadLen_),
                    static_cast<size_t>(sizeof(capturedPayload_) - capturedPayloadLen_)
                }).value_or(kReadFailed);
                if (n == kReadFailed || n == 0) {
                    break;
                }
                capturedPayloadLen_ += static_cast<int>(n);
            }
        } else if (consumeReadyRead_) {
            uint8_t buf[64];
            constexpr uint32_t kReadFailed = UINT32_MAX;
            while (true) {
                const uint32_t n = driver_->read({buf, sizeof(buf)}).value_or(kReadFailed);
                if (n == kReadFailed || n == 0) {
                    break;
                }
            }
        }

        if (disposeAfterReadyReads_ > 0 && readyReadCount_ >= disposeAfterReadyReads_) {
            DisposeAndStop();
            return;
        }

        if (disposeOnReadyRead_) {
            DisposeAndStop();
            return;
        }

        if (stopOnReadyRead_) {
            StopReactor();
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
            DisposeAndStop();
            return;
        }

        if (disposeAfterCommits_ > 0 && commitCount_ >= disposeAfterCommits_ && error == 0) {
            DisposeAndStop();
            return;
        }

        if (stopOnCommit_) {
            StopReactor();
        }
    }

    void onConnect(const int32_t error) noexcept
    {
        connectCount_++;
        lastConnectError_ = error;

        if (peerConnected_ != nullptr && error == 0) {
            peerConnected_->store(true);
        }

        if (error == 0 && secondConnectAddress_ != nullptr) {
            const auto second = driver_->connect(*secondConnectAddress_);
            secondConnectAttempted_ = true;
            secondConnectOk_ = second.has_value();
            if (!second) {
                secondConnectError_ = second.error();
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
            DisposeAndStop();
            return;
        }

        if (disposeOnConnect_ && error == 0) {
            DisposeAndStop();
            return;
        }

        if (scheduleDisposeOnConnectMs_ >= 0 && error == 0) {
            ScheduleDispose(scheduleDisposeOnConnectMs_);
            scheduleDisposeOnConnectMs_ = -1;
            return;
        }

        if (error == 0 && sendOnConnectAttempted_ && sendOnConnectOk_
            && !disposeOnConnect_ && !stopOnConnect_) {
            return;
        }

        if (stopOnConnect_) {
            StopReactor();
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

        if (stopOnDisconnect_) {
            StopReactor();
        }
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

        if (stopOnDisposed_) {
            if (peerDelegate_ == nullptr || peerDelegate_->disposedCount_ > 0) {
                StopReactor();
            }
        }
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
        actionTimer_.reset();
        driver_.reset();
        timeoutTimer_.reset();
        peerConnected_ = nullptr;
        reconnectAddress_ = nullptr;
        sendOnConnect2_ = nullptr;
        peerDelegate_ = nullptr;
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

    void StopReactor() noexcept
    {
        reactor_.shutdown();
    }

    void DisposeAndStop() noexcept
    {
        stopOnDisposed_ = true;
        driver_->dispose();
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

    [[nodiscard]] bool MakeAddress(const uint16_t port, C_Address& out) noexcept
    {
        return out.initialize("127.0.0.1", port).has_value();
    }

    void PrepareSend(send_operation_t& operation, uint8_t* data, const size_t len) noexcept
    {
        operation.content = etl::span<const uint8_t>{data, len};
        operation.transferred = 0;
    }

    void OnActionTimer() noexcept
    {
        if (actionDispose_) {
            actionDispose_ = false;
            DisposeAndStop();
        }
    }

    void OnTimeout() noexcept
    {
        timedOut_ = true;
        StopReactor();
    }

    C_Reactor reactor_{};
    std::optional<Driver> driver_;
    std::optional<C_Timer> timeoutTimer_;
    std::optional<C_Timer> actionTimer_;
    std::atomic<bool>* peerConnected_{nullptr};
    const C_Address* reconnectAddress_{nullptr};
    const C_Address* secondConnectAddress_{nullptr};

    int readyReadCount_{0};
    int connectCount_{0};
    int disconnectCount_{0};
    int disposedCount_{0};
    int commitCount_{0};
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
    SecondPeerDelegate* peerDelegate_{nullptr};
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

    bool stopOnReadyRead_{false};
    bool stopOnConnect_{false};
    bool stopOnDisconnect_{false};
    bool stopOnDisposed_{false};
    bool stopOnCommit_{false};
    bool disposeOnConnect_{false};
    bool disposeOnReadyRead_{false};
    bool disposeOnCommit_{false};
    bool consumeReadyRead_{false};
    bool captureReadyReadPayload_{false};
    bool reconnectOnConnectError_{false};
    bool reconnectOnDisposed_{false};
    bool reconnectOnDisconnect_{false};
    int disposeAfterReadyReads_{0};
    int disposeAfterConnects_{0};
    int disposeAfterCommits_{0};
    int scheduleDisposeOnConnectMs_{-1};
    char capturedPayload_[64]{};
    int capturedPayloadLen_{0};
};

TEST_F(TCPConnectionIOCPTest, DisposeWithoutConnectInvokesOnDisposed)
{
    stopOnDisposed_ = true;
    driver_->dispose();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(connectCount_, 0);
    EXPECT_EQ(disconnectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, DoubleDisposeInvokesOnDisposedOnce)
{
    stopOnDisposed_ = true;
    driver_->dispose();
    driver_->dispose();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
}


TEST_F(TCPConnectionIOCPTest, ConnectToListeningPeerInvokesOnConnectSuccess)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    disposeOnConnect_ = true;

    std::atomic<bool> accepted{false};
    std::thread acceptThread([&] {
        accepted.store(listener.Accept(2000));
    });

    const auto connectResult = driver_->connect(addr);
    ASSERT_TRUE(connectResult.has_value())
        << "connect error: " << connectResult.error()
        << ", family=" << static_cast<int>(addr.data().sa_family)
        << ", size=" << addr.size() << ", port=" << listener.Port();
    RunReactor();
    acceptThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(accepted.load());
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, SecondConnectWhileConnectingReturnsAlready)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    disposeOnConnect_ = true;

    std::thread acceptThread([&] {
        (void)listener.Accept(2000);
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());

    const auto second = driver_->connect(addr);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), static_cast<int32_t>(WSAEALREADY));

    RunReactor();
    acceptThread.join();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, SecondConnectWhileConnectedReturnsAlready)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    secondConnectAddress_ = &addr;
    disposeOnConnect_ = true;

    std::thread acceptThread([&] {
        (void)listener.Accept(2000);
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    acceptThread.join();

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
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    ASSERT_TRUE(driver_->connect(addr).has_value());
    driver_->dispose();

    const auto again = driver_->connect(addr);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), static_cast<int32_t>(WSAEALREADY));

    stopOnDisposed_ = true;
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(connectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, ConnectRefusedInvokesOnConnectWithError)
{
    LocalListener closed;
    ASSERT_TRUE(closed.Start());
    const auto port = closed.Port();
    closed.Close();

    C_Address addr;
    ASSERT_TRUE(MakeAddress(port, addr));

    stopOnConnect_ = true;
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
    LocalListener closed;
    ASSERT_TRUE(closed.Start());
    const auto deadPort = closed.Port();
    closed.Close();

    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address deadAddr;
    C_Address liveAddr;
    ASSERT_TRUE(MakeAddress(deadPort, deadAddr));
    ASSERT_TRUE(MakeAddress(listener.Port(), liveAddr));

    reconnectAddress_ = &liveAddr;
    reconnectOnConnectError_ = true;
    disposeOnConnect_ = true;

    std::thread acceptThread([&] {
        (void)listener.Accept(2000);
    });

    ASSERT_TRUE(driver_->connect(deadAddr).has_value());
    RunReactor();
    acceptThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(reconnectAttempted_);
    EXPECT_TRUE(reconnectOk_);
    ASSERT_EQ(connectCount_, 2);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, ReconnectAfterDisposeSucceeds)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    reconnectAddress_ = &addr;
    reconnectOnDisposed_ = true;
    disposeOnConnect_ = true;
    stopOnDisposed_ = true;

    std::thread acceptThread([&] {
        (void)listener.Accept(2000);
        (void)listener.Accept(2000);
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    acceptThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(reconnectAttempted_);
    EXPECT_TRUE(reconnectOk_);
    EXPECT_EQ(connectCount_, 2);
    EXPECT_EQ(disposedCount_, 2);
}

TEST_F(TCPConnectionIOCPTest, ReconnectFromOnDisconnectSucceeds)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    peerConnected_ = &connected;
    consumeReadyRead_ = true;
    reconnectAddress_ = &addr;
    reconnectOnDisconnect_ = true;
    disposeAfterConnects_ = 2;

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        while (!connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        listener.CloseAccepted();
        (void)listener.Accept(2000);
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

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
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    ASSERT_TRUE(driver_->connect(addr).has_value());

    stopOnDisposed_ = true;
    driver_->dispose();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);
    EXPECT_EQ(connectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, DisposeWhileConnectedInvokesOnDisposed)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    scheduleDisposeOnConnectMs_ = 10;

    std::thread acceptThread([&] {
        (void)listener.Accept(2000);
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    acceptThread.join();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, PeerDataInvokesOnReadyRead)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    std::atomic<bool> sent{false};
    peerConnected_ = &connected;
    disposeOnReadyRead_ = true;

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        while (!connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const char payload[] = "ping";
        sent.store(listener.SendAll(payload, static_cast<int>(sizeof(payload) - 1)));
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sent.load());
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_GE(readyReadCount_, 1);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, PeerDataReadableExactPayload)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    static constexpr char kPayload[] = "hello-etsl";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);

    std::atomic<bool> connected{false};
    std::atomic<bool> sent{false};
    peerConnected_ = &connected;
    captureReadyReadPayload_ = true;
    disposeOnReadyRead_ = true;

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        while (!connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        sent.store(listener.SendAll(kPayload, kPayloadLen));
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sent.load());
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, kPayloadLen);
    EXPECT_EQ(std::memcmp(capturedPayload_, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, PeerSendsWhileConnectInProgressIsDelivered)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    static constexpr char kPayload[] = "early";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);

    captureReadyReadPayload_ = true;
    disposeOnReadyRead_ = true;

    std::atomic<bool> sent{false};
    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        // Send immediately after accept, without waiting for the driver's onConnect:
        // the payload must be buffered by the stack and delivered once the read probe arms.
        sent.store(listener.SendAll(kPayload, kPayloadLen));
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sent.load());
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    ASSERT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, kPayloadLen);
    EXPECT_EQ(std::memcmp(capturedPayload_, kPayload, static_cast<size_t>(kPayloadLen)), 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, ReadyReadRearmsWhileDataUnread)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    peerConnected_ = &connected;
    disposeAfterReadyReads_ = 2;

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        while (!connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const char payload[] = "ping";
        (void)listener.SendAll(payload, static_cast<int>(sizeof(payload) - 1));
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_GE(readyReadCount_, 2);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, PeerGracefulCloseInvokesOnDisconnect)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    peerConnected_ = &connected;
    // EOF is observed only via driver.read(); that starts teardown → onDisconnect(0).
    consumeReadyRead_ = true;
    stopOnDisconnect_ = true;

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        while (!connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        listener.CloseAccepted();
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    ASSERT_EQ(disconnectCount_, 1);
    EXPECT_EQ(lastDisconnectError_, 0);
    EXPECT_EQ(disposedCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, PeerAbortiveCloseInvokesOnDisconnectWithError)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    peerConnected_ = &connected;
    consumeReadyRead_ = true;
    stopOnDisconnect_ = true;

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        while (!connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        listener.AbortAccepted();
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    ASSERT_EQ(disconnectCount_, 1);
    EXPECT_NE(lastDisconnectError_, 0);
    EXPECT_EQ(disposedCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, PeerDataThenGracefulClose)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    peerConnected_ = &connected;
    consumeReadyRead_ = true;
    stopOnDisconnect_ = true;

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        while (!connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const char payload[] = "bye";
        (void)listener.SendAll(payload, static_cast<int>(sizeof(payload) - 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        listener.CloseAccepted();
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_GE(readyReadCount_, 1);
    ASSERT_EQ(disconnectCount_, 1);
    EXPECT_EQ(lastDisconnectError_, 0);
    EXPECT_EQ(disposedCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, ExplicitDisposeAfterReadyReadDoesNotEmitDisconnect)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    peerConnected_ = &connected;
    disposeOnReadyRead_ = true;

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        while (!connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const char payload[] = "x";
        (void)listener.SendAll(payload, 1);
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

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

TEST_F(TCPConnectionIOCPTest, SendBeforeConnectReturnsInvalid)
{
    uint8_t bytes[] = {'x'};
    send_operation_t op{};
    PrepareSend(op, bytes, sizeof(bytes));

    const auto result = driver_->send(op.content, op);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAEINVAL));
    EXPECT_EQ(commitCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, SendAfterDisconnectReturnsInvalid)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    peerConnected_ = &connected;
    consumeReadyRead_ = true;
    stopOnDisconnect_ = true;

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }

        while (!connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        listener.CloseAccepted();
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    ASSERT_EQ(disconnectCount_, 1);

    uint8_t bytes[] = {'x'};
    send_operation_t op{};
    PrepareSend(op, bytes, sizeof(bytes));

    const auto result = driver_->send(op.content, op);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAEINVAL));
    EXPECT_EQ(commitCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, SendAfterDisposeReturnsInvalid)
{
    stopOnDisposed_ = true;
    driver_->dispose();
    RunReactor();
    ASSERT_EQ(disposedCount_, 1);

    uint8_t bytes[] = {'x'};
    send_operation_t op{};
    PrepareSend(op, bytes, sizeof(bytes));

    const auto result = driver_->send(op.content, op);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAEINVAL));
    EXPECT_EQ(commitCount_, 0);
}

TEST_F(TCPConnectionIOCPTest, SendAfterConnectInvokesOnCommitAndDeliversPayload)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    static constexpr char kPayload[] = "send-payload";
    static constexpr int kPayloadLen = static_cast<int>(sizeof(kPayload) - 1);
    uint8_t bytes[sizeof(kPayload) - 1];
    std::memcpy(bytes, kPayload, static_cast<size_t>(kPayloadLen));

    send_operation_t op{};
    PrepareSend(op, bytes, static_cast<size_t>(kPayloadLen));
    sendOnConnect_ = &op;
    disposeOnCommit_ = true;

    char received[sizeof(kPayload)]{};
    std::atomic<bool> gotPayload{false};

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }
        gotPayload.store(listener.RecvExact(received, kPayloadLen));
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sendOnConnectAttempted_);
    EXPECT_TRUE(sendOnConnectOk_);
    EXPECT_TRUE(gotPayload.load());
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
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    uint8_t firstBytes[] = {'A', 'A', 'A', 'A'};
    uint8_t secondBytes[] = {'B', 'B', 'B', 'B'};
    send_operation_t first{};
    send_operation_t second{};
    PrepareSend(first, firstBytes, sizeof(firstBytes));
    PrepareSend(second, secondBytes, sizeof(secondBytes));

    sendOnConnect_ = &first;
    sendOnCommit_ = &second;
    disposeOnCommit_ = true;

    char received[8]{};
    std::atomic<bool> gotPayload{false};

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }
        gotPayload.store(listener.RecvExact(received, 8));
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sendOnConnectOk_);
    EXPECT_TRUE(chainedSendAttempted_);
    EXPECT_TRUE(chainedSendOk_);
    EXPECT_TRUE(gotPayload.load());
    EXPECT_EQ(std::memcmp(received, "AAAABBBB", 8), 0);
    EXPECT_EQ(commitCount_, 2);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, ConcurrentSendOperationsAreDeliveredInOrder)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

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
    disposeAfterCommits_ = 2;

    char received[8]{};
    std::atomic<bool> gotPayload{false};

    std::thread peerThread([&] {
        if (!listener.Accept(2000)) {
            return;
        }
        gotPayload.store(listener.RecvExact(received, 8));
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(sendOnConnectOk_);
    EXPECT_TRUE(secondSendOnConnectOk_);
    EXPECT_TRUE(gotPayload.load());
    EXPECT_EQ(std::memcmp(received, "AAAABBBB", 8), 0);
    EXPECT_EQ(commitCount_, 2);
    EXPECT_EQ(lastCommitError_, 0);
    EXPECT_EQ(first.transferred, static_cast<uint32_t>(sizeof(firstBytes)));
    EXPECT_EQ(second.transferred, static_cast<uint32_t>(sizeof(secondBytes)));
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPConnectionIOCPTest, DisposeDuringPendingSendInvokesOnDisposed)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::vector<uint8_t> bytes(64 * 1024, static_cast<uint8_t>('Z'));
    send_operation_t op{};
    PrepareSend(op, bytes.data(), bytes.size());
    sendOnConnect_ = &op;
    disposeOnConnect_ = true;

    std::thread acceptThread([&] {
        (void)listener.Accept(2000);
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    acceptThread.join();

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
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    static constexpr char kFirstPayload[] = "first";
    uint8_t firstBytes[sizeof(kFirstPayload) - 1];
    std::memcpy(firstBytes, kFirstPayload, sizeof(firstBytes));

    send_operation_t first{};
    PrepareSend(first, firstBytes, sizeof(firstBytes));
    sendOnConnect_ = &first;
    captureReadyReadPayload_ = true;
    disposeAfterReadyReads_ = 1;

    std::atomic<bool> payloadsOk{false};

    SecondPeerDelegate second(reactor_, &disposedCount_);
    peerDelegate_ = &second;

    // Connect the fixture driver first: the peer thread identifies connections
    // by accept order, and ConnectEx issues SYNs in call order on loopback.
    ASSERT_TRUE(driver_->connect(addr).has_value());
    second.Start(addr);

    std::thread peerThread([&] {
        const SOCKET a = listener.AcceptRaw(2000);
        const SOCKET b = listener.AcceptRaw(2000);
        if (a == INVALID_SOCKET || b == INVALID_SOCKET) {
            return;
        }

        char firstReceived[sizeof(kFirstPayload) - 1];
        char secondReceived[4];
        if (!listener.RecvExactOn(a, firstReceived, static_cast<int>(sizeof(firstReceived)))) {
            return;
        }
        if (!listener.RecvExactOn(b, secondReceived, sizeof(secondReceived))) {
            return;
        }

        payloadsOk.store(std::memcmp(firstReceived, kFirstPayload, sizeof(firstReceived)) == 0
            && std::memcmp(secondReceived, "2nd!", sizeof(secondReceived)) == 0);

        (void)listener.SendAllOn(a, "echoA", 5);
        (void)listener.SendAllOn(b, "echoB", 5);
        ::shutdown(a, SD_BOTH);
        ::closesocket(a);
        ::shutdown(b, SD_BOTH);
        ::closesocket(b);
    });

    RunReactor();
    peerThread.join();

    EXPECT_FALSE(timedOut_);
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    ASSERT_GE(readyReadCount_, 1);
    ASSERT_EQ(capturedPayloadLen_, 5);
    EXPECT_EQ(std::memcmp(capturedPayload_, "echoA", 5), 0);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(disconnectCount_, 0);

    EXPECT_TRUE(second.startedOk_);
    ASSERT_EQ(second.connectCount_, 1);
    EXPECT_EQ(second.lastConnectError_, 0);
    EXPECT_TRUE(second.sendOk_);
    ASSERT_EQ(second.payloadLen_, 5);
    EXPECT_EQ(std::memcmp(second.payload_, "echoB", 5), 0);
    EXPECT_EQ(second.lastCommitError_, 0);
    ASSERT_EQ(second.disposedCount_, 1);
    EXPECT_EQ(second.disconnectCount_, 0);
    EXPECT_TRUE(payloadsOk.load());
}

TEST_F(TCPConnectionIOCPTest, SendWhileConnectingReturnsInvalid)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    disposeOnConnect_ = true;

    std::thread acceptThread([&] {
        (void)listener.Accept(2000);
    });

    ASSERT_TRUE(driver_->connect(addr).has_value());

    uint8_t bytes[] = {'x'};
    send_operation_t op{};
    PrepareSend(op, bytes, sizeof(bytes));
    const auto result = driver_->send(op.content, op);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), static_cast<int32_t>(WSAEINVAL));

    RunReactor();
    acceptThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(commitCount_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

} // namespace
