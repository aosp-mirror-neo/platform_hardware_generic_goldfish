// Copyright (C) 2025 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#include "goldfish/devices/unix_pipe/unix_pipe.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <future>
#include <thread>

#include "absl/status/status_matchers.h"
#include "absl/synchronization/mutex.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "android/base/testing/test_system.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/libuv_socket_factory.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/devices/test_connector_registry.h"
#include "goldfish/network/endpoint.h"
#include "goldfish/network/un_endpoint.h"

namespace goldfish::devices::unix_pipe {

using absl_testing::IsOk;
using testing::Not;

using android::base::TestSystem;
using async::testing::TestEventLoop;
using goldfish::async::AsyncSocket;
using goldfish::async::LibuvAsyncSocketFactory;
using goldfish::async::LibuvEventLoop;
using goldfish::async::ScopedAsyncServer;
using goldfish::async::ScopedAsyncSocket;
using goldfish::network::Endpoint;
using goldfish::network::UnEndpoint;

using namespace std::chrono_literals;
using namespace std::literals::string_view_literals;

class UnixPipeTest : public ::testing::Test {
    void SetUp() override {
        client_loop_ = LibuvEventLoop::Create();
        client_loop_thread_ = std::thread([this] { (void)client_loop_->Run(); });
        qemu_loop_ = TestEventLoop::Create();
        IUnixPipe::RegisterDevice(&registry_, client_loop_.get(), qemu_loop_.get());
    }

    void TearDown() override {
        registry_.Close();
        auto s = client_loop_->ShutdownAndWait(absl::Seconds(10));
        EXPECT_THAT(s, absl_testing::IsOk());
        if (client_loop_thread_.joinable()) {
            client_loop_thread_.join();
        }
    }

  public:
    template <typename F>
    auto PostAndWait(F&& func) -> decltype(func()) {
        auto res = client_loop_->PostAndWait(std::forward<F>(func));
        EXPECT_THAT(res, absl_testing::IsOk());
        if constexpr (std::is_void_v<decltype(func())>) {
            return;
        } else {
            return *res;
        }
    }

    template <typename T>
    void RunUntil(std::future<T>& future, std::chrono::milliseconds timeout = 10s) {
        ASSERT_EQ(future.wait_for(timeout), std::future_status::ready);
    }

    // Waits until the device closes the guest connection.
    bool WaitForClose(TestHalSocket* socket, absl::Duration timeout = absl::Seconds(10)) {
        const absl::Time deadline = absl::Now() + timeout;
        while (absl::Now() < deadline) {
            if (PostAndWait([socket] { return socket->closed; })) {
                return true;
            }
            absl::SleepFor(absl::Milliseconds(10));
        }
        return false;
    }

  protected:
    std::unique_ptr<LibuvEventLoop> client_loop_;
    std::thread client_loop_thread_;
    std::unique_ptr<TestEventLoop> qemu_loop_;
    TestConnectorRegistry registry_;
    LibuvAsyncSocketFactory socket_factory_;
};

struct DeviceState {
    std::string rx_buffer;
    bool disconnected = false;

    bool ready() const { return !rx_buffer.empty() || disconnected; }
};

TEST_F(UnixPipeTest, echo_over_host_side) {
#ifdef _WIN32
    const std::string un_path(absl::StrFormat("\\\\.\\pipe\\UnixPipeTest_echo_over_host_side_%d",
                                              GetCurrentProcessId()));
#else
    android::base::TestTempDir tmpdir("UnixPipeTest");
    const std::filesystem::path un_path = tmpdir.MakeSubPath("echo_over_host_side");
#endif

    const auto endpoint = Endpoint(*UnEndpoint::Create(un_path));

    std::vector<ScopedAsyncSocket> clients;
    std::mutex clients_mutex;

    auto on_connect = [&](std::shared_ptr<AsyncSocket> socket) -> bool {
        const std::weak_ptr<AsyncSocket> weak_socket(socket);

        socket->SetOnReadCallbackNoFlowControl(
                [weak_socket](std::string_view data, absl::Status /*err*/) {
                    if (const auto socket = weak_socket.lock()) {
                        ASSERT_THAT(socket->Send(data.data(), data.size()), IsOk());
                    }
                });

        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.emplace_back(std::move(socket));
        return true;
    };

    const ScopedAsyncServer server(PostAndWait([this, &endpoint, &on_connect] {
        return socket_factory_.CreateServer(client_loop_.get(), endpoint, std::move(on_connect));
    }));
    ASSERT_NE(server, nullptr);

    IUnixPipe* device = PostAndWait([this, &endpoint] {
        return registry_.ConstructHalDevice<IUnixPipe>(ToString(endpoint));
    });
    ASSERT_NE(device, nullptr);
    PostAndWait([device] { device->OnConnect(); });
    TestHalSocket* device_socket = registry_.HalSocket();
    ASSERT_NE(device_socket, nullptr);

    absl::Mutex device_updated;
    DeviceState device_state;

    device_socket->on_send = [&](std::string_view data) {
        const absl::MutexLock lk(&device_updated);
        if (data.empty()) {
            device_state.disconnected = true;
        } else {
            device_state.rx_buffer.append(data);
        }
    };

    const std::string test_message = "test message";

    client_loop_->PostAndWait([&]() { device->OnReceive(test_message); }).IgnoreError();

    const absl::MutexLock lk(&device_updated);
    ASSERT_TRUE(device_updated.AwaitWithTimeout(
            absl::Condition(
                    +[](const void* ptr) { return static_cast<const DeviceState*>(ptr)->ready(); },
                    &device_state),
            absl::Seconds(10)));

    EXPECT_EQ(device_state.rx_buffer, test_message);

    device_socket->on_send = [](std::string_view) {};
}

TEST_F(UnixPipeTest, close_on_host) {
#ifdef _WIN32
    const std::string un_path(
            absl::StrFormat("\\\\.\\pipe\\UnixPipeTest_%d", GetCurrentProcessId()));
#else
    android::base::TestTempDir tmpdir("UnixPipeTest");
    const std::filesystem::path un_path = tmpdir.MakeSubPath("close_on_host");
#endif

    const auto endpoint = Endpoint(*UnEndpoint::Create(un_path));

    std::vector<ScopedAsyncSocket> clients;
    std::mutex clients_mutex;
    int server_replies = 3;

    auto on_connect = [&](std::shared_ptr<AsyncSocket> socket) -> bool {
        const std::weak_ptr<AsyncSocket> weak_socket(socket);

        socket->SetOnReadCallbackNoFlowControl(
                [weak_socket, &server_replies](std::string_view data, absl::Status /*err*/) {
                    if (const auto socket = weak_socket.lock()) {
                        if (server_replies > 0) {
                            --server_replies;
                            ASSERT_THAT(socket->Send(data.data(), data.size()), IsOk());
                        } else {
                            socket->Close();
                        }
                    }
                });

        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.emplace_back(std::move(socket));
        return true;
    };

    const ScopedAsyncServer server(PostAndWait([this, &endpoint, &on_connect] {
        return socket_factory_.CreateServer(client_loop_.get(), endpoint, std::move(on_connect));
    }));
    ASSERT_NE(server, nullptr);

    IUnixPipe* device = registry_.ConstructHalDevice<IUnixPipe>(ToString(endpoint));
    ASSERT_NE(device, nullptr);
    PostAndWait([device] { device->OnConnect(); });
    TestHalSocket* device_socket = registry_.HalSocket();
    ASSERT_NE(device_socket, nullptr);

    std::string rx_total;
    absl::Mutex device_updated;
    DeviceState device_state;

    device_socket->on_send = [&](std::string_view data) {
        const absl::MutexLock lk(&device_updated);
        if (data.empty()) {
            device_state.disconnected = true;
        } else {
            device_state.rx_buffer.append(data);
        }
    };

    const std::string test_message = "abc";

    for (int n = 3; n > 0; --n) {
        client_loop_->PostAndWait([&]() { device->OnReceive(test_message); }).IgnoreError();

        const absl::MutexLock lk(&device_updated);
        ASSERT_TRUE(device_updated.AwaitWithTimeout(
                absl::Condition(
                        +[](const void* ptr) {
                            return static_cast<const DeviceState*>(ptr)->ready();
                        },
                        &device_state),
                absl::Seconds(10)));

        rx_total.append(device_state.rx_buffer);
        device_state.rx_buffer.clear();
    }

    EXPECT_EQ(rx_total, "abcabcabc");

    client_loop_->PostAndWait([&]() { device->OnReceive(test_message); }).IgnoreError();

    ASSERT_TRUE(WaitForClose(device_socket));
    {
        const absl::MutexLock lk(&device_updated);
        EXPECT_TRUE(device_state.rx_buffer.empty());
    }
    device_socket->on_send = [](std::string_view) {};
}

// Captures the HAL device factory so a test can drive the real connection lifecycle instead of
// the one TestConnectorRegistry::ConstructHalDevice() uses. In the emulator,
// HalPlugFactory::WrapHalPlug() runs the factory on the QEMU thread, then calls
// EstablishConnection(), and only then posts OnConnect() to the client loop.
class LifecycleConnectorRegistry : public TestConnectorRegistry {
  public:
    void RegisterHalDevice(std::string name, async::EventLoop* client_loop,
                           async::EventLoop* qemu_loop, HalDeviceFactory factory) override {
        factory_ = factory;
        TestConnectorRegistry::RegisterHalDevice(std::move(name), client_loop, qemu_loop,
                                                 std::move(factory));
    }

    HalDeviceFactory factory_;
};

// The host side may write as soon as it accepts the AF_UNIX
// connection, which can happen before the guest connection is established. That data must reach
// the guest once OnConnect() is delivered instead of being written to the null socket.
TEST_F(UnixPipeTest, host_data_sent_before_on_connect_reaches_guest) {
#ifdef _WIN32
    const std::string un_path(absl::StrFormat(
            "\\\\.\\pipe\\UnixPipeTest_host_data_before_on_connect_%d", GetCurrentProcessId()));
#else
    android::base::TestTempDir tmpdir("UnixPipeTest");
    const std::filesystem::path un_path = tmpdir.MakeSubPath("host_data_before_on_connect");
#endif

    const auto endpoint = Endpoint(*UnEndpoint::Create(un_path));

    LifecycleConnectorRegistry registry;
    IUnixPipe::RegisterDevice(&registry, client_loop_.get(), qemu_loop_.get());
    ASSERT_TRUE(registry.factory_);

    std::vector<ScopedAsyncSocket> clients;
    std::mutex clients_mutex;
    std::promise<void> greeted;
    auto greeted_future = greeted.get_future();

    // Greet immediately on accept, like a host that starts with a handshake.
    auto on_connect = [&](std::shared_ptr<AsyncSocket> socket) -> bool {
        socket->SetOnReadCallbackNoFlowControl([](std::string_view, absl::Status) {});
        EXPECT_THAT(socket->Send("rdy", 3), IsOk());
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.emplace_back(std::move(socket));
        }
        greeted.set_value();
        return true;
    };

    const ScopedAsyncServer server(PostAndWait([this, &endpoint, &on_connect] {
        return socket_factory_.CreateServer(client_loop_.get(), endpoint, std::move(on_connect));
    }));
    ASSERT_NE(server, nullptr);

    // The factory runs on the QEMU thread. The guest connection is not established yet.
    std::shared_ptr<HalPlug> plug = registry.factory_(ToString(endpoint));
    ASSERT_NE(plug, nullptr);

    RunUntil(greeted_future);
    // Let the client loop read the greeting while the plug still has the null socket.
    std::this_thread::sleep_for(100ms);
    PostAndWait([] {});

    absl::Mutex device_updated;
    DeviceState device_state;
    auto hal_socket = std::make_shared<TestHalSocket>();
    hal_socket->on_send = [&](std::string_view data) {
        const absl::MutexLock lk(&device_updated);
        if (data.empty()) {
            device_state.disconnected = true;
        } else {
            device_state.rx_buffer.append(data);
        }
    };

    HalPlugTesting::EstablishConnection(plug.get(), hal_socket);
    client_loop_->Post([plug] { plug->OnConnect(); }).IgnoreError();

    {
        const absl::MutexLock lk(&device_updated);
        EXPECT_TRUE(device_updated.AwaitWithTimeout(
                absl::Condition(
                        +[](const void* ptr) {
                            return static_cast<const DeviceState*>(ptr)->ready();
                        },
                        &device_state),
                absl::Seconds(10)));
        EXPECT_EQ(device_state.rx_buffer, "rdy");
    }

    PostAndWait([&plug] { plug->OnClose(); });
    hal_socket->on_send = [](std::string_view) {};
}

// If the host sends data and closes the AF_UNIX connection before OnConnect(), the guest must
// still get the data and then have its connection closed.
TEST_F(UnixPipeTest, host_close_before_on_connect_closes_guest) {
#ifdef _WIN32
    const std::string un_path(absl::StrFormat(
            "\\\\.\\pipe\\UnixPipeTest_host_close_before_on_connect_%d", GetCurrentProcessId()));
#else
    android::base::TestTempDir tmpdir("UnixPipeTest");
    const std::filesystem::path un_path = tmpdir.MakeSubPath("host_close_before_on_connect");
#endif

    const auto endpoint = Endpoint(*UnEndpoint::Create(un_path));

    LifecycleConnectorRegistry registry;
    IUnixPipe::RegisterDevice(&registry, client_loop_.get(), qemu_loop_.get());
    ASSERT_TRUE(registry.factory_);

    std::promise<void> closed;
    auto closed_future = closed.get_future();

    // Send a message and close right away.
    auto on_connect = [&](std::shared_ptr<AsyncSocket> socket) -> bool {
        socket->SetOnReadCallbackNoFlowControl([](std::string_view, absl::Status) {});
        EXPECT_THAT(socket->Send("bye", 3), IsOk());
        socket->Close();
        closed.set_value();
        return true;
    };

    const ScopedAsyncServer server(PostAndWait([this, &endpoint, &on_connect] {
        return socket_factory_.CreateServer(client_loop_.get(), endpoint, std::move(on_connect));
    }));
    ASSERT_NE(server, nullptr);

    std::shared_ptr<HalPlug> plug = registry.factory_(ToString(endpoint));
    ASSERT_NE(plug, nullptr);

    RunUntil(closed_future);
    // Let the client loop see the data and the close while the plug still has the null socket.
    std::this_thread::sleep_for(100ms);
    PostAndWait([] {});

    std::string rx_buffer;
    auto hal_socket = std::make_shared<TestHalSocket>();
    hal_socket->on_send = [&](std::string_view data) { rx_buffer.append(data); };

    HalPlugTesting::EstablishConnection(plug.get(), hal_socket);
    PostAndWait([plug] { plug->OnConnect(); });

    ASSERT_TRUE(WaitForClose(hal_socket.get()));
    EXPECT_EQ(PostAndWait([&rx_buffer] { return rx_buffer; }), "bye");

    PostAndWait([&plug] { plug->OnClose(); });
    hal_socket->on_send = [](std::string_view) {};
}

}  // namespace goldfish::devices::unix_pipe
