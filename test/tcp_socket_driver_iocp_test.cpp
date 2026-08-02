#include <gtest/gtest.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <etl/chrono.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>

import reactor;
import timer;
import socket.address;
import socket.types;
import net.tcp_socket_driver.types;
import net.tcp_socket_driver.iocp;

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

class TCPSocketDriverIOCPTest : public ::testing::Test
{
protected:
    using on_ready_read_callback_t = etsl::on_ready_read_callback_t;
    using on_commit_callback_t = etsl::on_commit_callback_t;
    using on_connect_callback_t = etsl::on_connect_callback_t;
    using on_disconnect_callback_t = etsl::on_disconnect_callback_t;
    using on_disposed_callback_t = etsl::on_disposed_callback_t;
    using clock_t = etsl::clock_t;
    using timer_callback_t = etsl::timer_callback_t;
    using socket_t = etsl::socket_t;
    using C_Address = etsl::C_Address;
    using C_Reactor = etsl::C_Reactor;
    using C_Timer = etsl::C_Timer;
    using C_TCPSocketDriverIOCP = etsl::C_TCPSocketDriverIOCP;
    using tcp_socket_driver_events_s = etsl::tcp_socket_driver_events_s;

    void SetUp() override
    {
        ASSERT_TRUE(reactor_.initialize().has_value());

        events_.onReadyRead = on_ready_read_callback_t::create<TCPSocketDriverIOCPTest,
            &TCPSocketDriverIOCPTest::OnReadyRead>(*this);
        events_.onCommit = on_commit_callback_t::create<TCPSocketDriverIOCPTest,
            &TCPSocketDriverIOCPTest::OnCommit>(*this);
        events_.onConnect = on_connect_callback_t::create<TCPSocketDriverIOCPTest,
            &TCPSocketDriverIOCPTest::OnConnect>(*this);
        events_.onDisconnect = on_disconnect_callback_t::create<TCPSocketDriverIOCPTest,
            &TCPSocketDriverIOCPTest::OnDisconnect>(*this);
        events_.onDisposed = on_disposed_callback_t::create<TCPSocketDriverIOCPTest,
            &TCPSocketDriverIOCPTest::OnDisposed>(*this);

        driver_.emplace(reactor_, events_);
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
    }

    void RunReactor(const int32_t timeoutMs = 3000)
    {
        timedOut_ = false;
        timeoutTimer_.emplace(timer_callback_t::create<TCPSocketDriverIOCPTest,
            &TCPSocketDriverIOCPTest::OnTimeout>(*this));

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
        actionTimer_.emplace(timer_callback_t::create<TCPSocketDriverIOCPTest,
            &TCPSocketDriverIOCPTest::OnActionTimer>(*this));
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

    void OnReadyRead(socket_t fd) noexcept
    {
        readyReadCount_++;
        lastReadyFd_ = fd;

        if (captureReadyReadPayload_) {
            capturedPayloadLen_ = 0;
            while (capturedPayloadLen_ < static_cast<int>(sizeof(capturedPayload_))) {
                const int n = ::recv(fd, capturedPayload_ + capturedPayloadLen_,
                    static_cast<int>(sizeof(capturedPayload_)) - capturedPayloadLen_, 0);
                if (n > 0) {
                    capturedPayloadLen_ += n;
                    continue;
                }
                break;
            }
        } else if (consumeReadyRead_) {
            char buf[64];
            while (::recv(fd, buf, sizeof(buf), 0) > 0) {
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

    void OnCommit(C_Reactor::operation_t&, int32_t) noexcept {}

    void OnConnect(const int32_t error) noexcept
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

        if (disposeOnConnect_ && error == 0) {
            DisposeAndStop();
            return;
        }

        if (scheduleDisposeOnConnectMs_ >= 0 && error == 0) {
            ScheduleDispose(scheduleDisposeOnConnectMs_);
            scheduleDisposeOnConnectMs_ = -1;
            return;
        }

        if (stopOnConnect_) {
            StopReactor();
        }
    }

    void OnDisconnect(const int32_t error) noexcept
    {
        disconnectCount_++;
        lastDisconnectError_ = error;

        if (stopOnDisconnect_) {
            StopReactor();
        }
    }

    void OnDisposed() noexcept
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
            StopReactor();
        }
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
    tcp_socket_driver_events_s events_{};
    std::optional<C_TCPSocketDriverIOCP> driver_;
    std::optional<C_Timer> timeoutTimer_;
    std::optional<C_Timer> actionTimer_;
    std::atomic<bool>* peerConnected_{nullptr};
    const C_Address* reconnectAddress_{nullptr};
    const C_Address* secondConnectAddress_{nullptr};

    int readyReadCount_{0};
    int connectCount_{0};
    int disconnectCount_{0};
    int disposedCount_{0};
    int32_t lastConnectError_{0};
    int32_t lastDisconnectError_{0};
    int32_t secondConnectError_{0};
    int32_t reconnectError_{0};
    socket_t lastReadyFd_{INVALID_SOCKET};
    bool timedOut_{false};
    bool secondConnectAttempted_{false};
    bool secondConnectOk_{false};
    bool reconnectAttempted_{false};
    bool reconnectOk_{false};
    bool actionDispose_{false};

    bool stopOnReadyRead_{false};
    bool stopOnConnect_{false};
    bool stopOnDisconnect_{false};
    bool stopOnDisposed_{false};
    bool disposeOnConnect_{false};
    bool disposeOnReadyRead_{false};
    bool consumeReadyRead_{false};
    bool captureReadyReadPayload_{false};
    bool reconnectOnConnectError_{false};
    bool reconnectOnDisposed_{false};
    int disposeAfterReadyReads_{0};
    int scheduleDisposeOnConnectMs_{-1};
    char capturedPayload_[64]{};
    int capturedPayloadLen_{0};
};

TEST_F(TCPSocketDriverIOCPTest, DisposeWithoutConnectInvokesOnDisposed)
{
    stopOnDisposed_ = true;
    driver_->dispose();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
    EXPECT_EQ(connectCount_, 0);
    EXPECT_EQ(disconnectCount_, 0);
}

TEST_F(TCPSocketDriverIOCPTest, DoubleDisposeInvokesOnDisposedOnce)
{
    stopOnDisposed_ = true;
    driver_->dispose();
    driver_->dispose();
    RunReactor();

    EXPECT_FALSE(timedOut_);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPSocketDriverIOCPTest, ConnectToListeningPeerInvokesOnConnectSuccess)
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

    ASSERT_TRUE(driver_->connect(addr).has_value());
    RunReactor();
    acceptThread.join();

    EXPECT_FALSE(timedOut_);
    EXPECT_TRUE(accepted.load());
    ASSERT_EQ(connectCount_, 1);
    EXPECT_EQ(lastConnectError_, 0);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPSocketDriverIOCPTest, SecondConnectWhileConnectingReturnsAlready)
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

TEST_F(TCPSocketDriverIOCPTest, SecondConnectWhileConnectedReturnsAlready)
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

TEST_F(TCPSocketDriverIOCPTest, ConnectWhileDisposingReturnsAlready)
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

TEST_F(TCPSocketDriverIOCPTest, ConnectRefusedInvokesOnConnectWithError)
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

TEST_F(TCPSocketDriverIOCPTest, ReconnectAfterConnectFailureSucceeds)
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

TEST_F(TCPSocketDriverIOCPTest, ReconnectAfterDisposeSucceeds)
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

TEST_F(TCPSocketDriverIOCPTest, DisposeDuringConnectInvokesOnDisposed)
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

TEST_F(TCPSocketDriverIOCPTest, DisposeWhileConnectedInvokesOnDisposed)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    // Dispose after connect routine finishes and read-probe is armed.
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

TEST_F(TCPSocketDriverIOCPTest, PeerDataInvokesOnReadyRead)
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
    EXPECT_NE(lastReadyFd_, INVALID_SOCKET);
    EXPECT_EQ(disposedCount_, 1);
}

TEST_F(TCPSocketDriverIOCPTest, PeerDataReadableExactPayload)
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

TEST_F(TCPSocketDriverIOCPTest, ReadyReadRearmsWhileDataUnread)
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

TEST_F(TCPSocketDriverIOCPTest, PeerGracefulCloseInvokesOnDisconnect)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    peerConnected_ = &connected;
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

TEST_F(TCPSocketDriverIOCPTest, PeerAbortiveCloseInvokesOnDisconnectWithError)
{
    LocalListener listener;
    ASSERT_TRUE(listener.Start());

    C_Address addr;
    ASSERT_TRUE(MakeAddress(listener.Port(), addr));

    std::atomic<bool> connected{false};
    peerConnected_ = &connected;
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

TEST_F(TCPSocketDriverIOCPTest, PeerDataThenGracefulClose)
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

TEST_F(TCPSocketDriverIOCPTest, ExplicitDisposeAfterReadyReadDoesNotEmitDisconnect)
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

} // namespace
