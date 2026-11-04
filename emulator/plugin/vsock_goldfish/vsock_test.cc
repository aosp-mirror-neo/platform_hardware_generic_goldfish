// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"

#include "android/base/testing/needs_winsock.h"
#include "goldfish/archive/reader.h"
#include "goldfish/archive/writer.h"
#include "goldfish/async/async_socket_server.h"
#include "goldfish/async/testing/fake_async_socket.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/devices/cable/cable.h"
#include "goldfish/network/endpoint.h"
#include "goldfish/network/ip_address.h"
#include "goldfish/vsock/clear.h"
#include "goldfish/vsock/connect.h"
#include "goldfish/vsock/vsock_low_level.h"
#include "vsock_port_fwd_internal.h"

struct QEMUFile {
    std::string buffer;
    size_t read_offset = 0;
};

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#include "qemu/osdep.h"
#include "standard-headers/linux/virtio_vsock.h"

size_t qemu_get_buffer(QEMUFile* f, uint8_t* buf, size_t size) {
    if (f == nullptr || f->read_offset >= f->buffer.size()) {
        return 0;
    }
    const size_t avail = std::min(size, f->buffer.size() - f->read_offset);
    std::memcpy(buf, f->buffer.data() + f->read_offset, avail);
    f->read_offset += avail;
    return avail;
}

void qemu_put_buffer(QEMUFile* f, const uint8_t* buf, size_t size) {
    if (f != nullptr && buf != nullptr && size > 0) {
        f->buffer.append(reinterpret_cast<const char*>(buf), size);
    }
}
}
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::vsock {
namespace {

using devices::cable::IPlug;
using devices::cable::PlugOrSocket;
using devices::cable::PlugPtr;
using devices::cable::RegisterPlugLoader;
using devices::cable::SocketPtr;

constexpr uint32_t kGuestPort = 5555;
constexpr uint64_t kGuestCid = 3;
constexpr uint64_t kHostCid = 2;
constexpr uint32_t kExpectedHostBufAlloc = 1024 * 1024;

struct CapturedPacket {
    struct virtio_vsock_hdr hdr;
    std::string payload;
};

class SnapshottablePlug : public IPlug {
  public:
    static constexpr const char* kTypeId = "vsock_test_snapshottable_plug";

    explicit SnapshottablePlug(uint32_t tag) : tag_(tag) {}

    void SetSocket(SocketPtr socket) { socket_ = std::move(socket); }
    devices::cable::ISocket* socket() const { return socket_.get(); }
    uint32_t tag() const { return tag_; }
    const std::string& received() const { return received_; }

    void OnConnect() override {}

    SocketPtr OnUnplug() override { return std::move(socket_); }

    bool OnReceive(const void* data, size_t size) override {
        received_.append(static_cast<const char*>(data), size);
        return true;
    }

    bool SupportsLoadingFromSnapshot() const override { return true; }

    TypeId GetSnapshotTypeId() const override { return kTypeId; }

    bool SaveStateToSnapshot(archive::IWriter& writer) const override {
        writer << tag_;
        return true;
    }

    static std::unordered_map<uint32_t, std::shared_ptr<SnapshottablePlug>>& RestoredPlugs() {
        static auto* restored =
                new std::unordered_map<uint32_t, std::shared_ptr<SnapshottablePlug>>();
        return *restored;
    }

    static void EnsureLoaderRegistered() {
        static const bool kRegistered = RegisterPlugLoader(
                kTypeId, [](SocketPtr socket, archive::IReader& reader) -> PlugOrSocket {
                    uint32_t tag = 0;
                    if (!archive::ReadValue(reader, tag).ok()) {
                        return std::move(socket);
                    }
                    auto plug = std::make_shared<SnapshottablePlug>(tag);
                    plug->SetSocket(std::move(socket));
                    RestoredPlugs()[tag] = plug;
                    return PlugPtr(plug);
                });
        (void)kRegistered;
    }

  private:
    uint32_t tag_ = 0;
    SocketPtr socket_;
    std::string received_;
};

class VsockSnapshotFlagsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        SnapshottablePlug::EnsureLoaderRegistered();
        SnapshottablePlug::RestoredPlugs().clear();
        static const GoldfishVirtIOVSockDevAPI kApi = {
            .haveHostToGuestPackets = &VsockSnapshotFlagsTest::OnHaveHostToGuestPackets,
            .sendPacketHostToGuest = &VsockSnapshotFlagsTest::OnSendPacketHostToGuest,
            .haveHostToGuestEvents = &VsockSnapshotFlagsTest::OnHaveHostToGuestEvents,
            .sendEventHostToGuest = &VsockSnapshotFlagsTest::OnSendEventHostToGuest,
        };
        impl_ = goldfish_virtio_vsock_impl_realize(this, &kApi);
    }

    void TearDown() override {
        SnapshottablePlug::RestoredPlugs().clear();
        clear();
        goldfish_virtio_vsock_impl_unrealize(impl_);
    }

    uint32_t CompleteHandshake(uint32_t guest_port, uint32_t guest_buf_alloc) {
        uint32_t host_port = 0;
        for (auto it = packets_.rbegin(); it != packets_.rend(); ++it) {
            if (it->hdr.op == VIRTIO_VSOCK_OP_REQUEST && it->hdr.dst_port == guest_port) {
                host_port = it->hdr.src_port;
                break;
            }
        }
        EXPECT_NE(host_port, 0U);

        struct virtio_vsock_hdr resp = {
            .src_cid = kGuestCid,
            .dst_cid = kHostCid,
            .src_port = guest_port,
            .dst_port = host_port,
            .len = 0,
            .type = VIRTIO_VSOCK_TYPE_STREAM,
            .op = VIRTIO_VSOCK_OP_RESPONSE,
            .flags = 0,
            .buf_alloc = guest_buf_alloc,
            .fwd_cnt = 0,
        };
        goldfish_virtio_vsock_accept_guest_to_host_control(impl_, &resp);
        return host_port;
    }

    static void OnHaveHostToGuestPackets(void* opaque) {
        static_cast<VsockSnapshotFlagsTest*>(opaque)->notify_count_++;
    }

    static VirtIOVSockSendResult OnSendPacketHostToGuest(void* opaque, struct virtio_vsock_hdr* hdr,
                                                         const void* data) {
        auto* self = static_cast<VsockSnapshotFlagsTest*>(opaque);
        if (self->vq_full_) {
            return MakeVirtIOVSockSendResult(/*is_vq_full=*/true, /*need_notify=*/false, 0);
        }
        const size_t len = hdr->len;
        CapturedPacket pkt;
        pkt.hdr = *hdr;
        if (data != nullptr && len > 0) {
            pkt.payload.assign(static_cast<const char*>(data), len);
        }
        self->packets_.push_back(std::move(pkt));
        return MakeVirtIOVSockSendResult(/*is_vq_full=*/false, /*need_notify=*/true, len);
    }

    static void OnHaveHostToGuestEvents(void* /*opaque*/) {}

    static VirtIOVSockSendResult OnSendEventHostToGuest(void* /*opaque*/,
                                                        const struct virtio_vsock_event* /*evt*/) {
        return MakeVirtIOVSockSendResult(false, false, 1);
    }

    void* impl_ = nullptr;
    bool vq_full_ = false;
    size_t notify_count_ = 0;
    std::vector<CapturedPacket> packets_;
};

TEST_F(VsockSnapshotFlagsTest, HighBitSendOpMaskDoesNotCorruptSnapshotStreamViaVarint) {
    auto plug = std::make_shared<SnapshottablePlug>(/*tag=*/101);
    SocketPtr sock = Connect(kGuestPort, plug);
    const uint32_t host_port = CompleteHandshake(kGuestPort, /*guest_buf_alloc=*/0);
    plug->SetSocket(std::move(sock));

    vq_full_ = true;
    const std::string kPayload = "hello";
    plug->socket()->SendAsync(kPayload.data(), kPayload.size());

    QEMUFile snapshot;
    ASSERT_EQ(goldfish_virtio_vsock_impl_save(impl_, &snapshot), 0);

    vq_full_ = false;
    packets_.clear();
    ASSERT_EQ(goldfish_virtio_vsock_impl_load(impl_, &snapshot), 0);

    ASSERT_EQ(SnapshottablePlug::RestoredPlugs().size(), 1U);
    ASSERT_TRUE(SnapshottablePlug::RestoredPlugs().count(101));

    // Grant guest credit and verify the buffered payload is transmitted intact
    // without a phantom varint byte.
    struct virtio_vsock_hdr credit = {
        .src_cid = kGuestCid,
        .dst_cid = kHostCid,
        .src_port = kGuestPort,
        .dst_port = host_port,
        .len = 0,
        .type = VIRTIO_VSOCK_TYPE_STREAM,
        .op = VIRTIO_VSOCK_OP_CREDIT_UPDATE,
        .flags = 0,
        .buf_alloc = 65536,
        .fwd_cnt = 0,
    };
    goldfish_virtio_vsock_accept_guest_to_host_control(impl_, &credit);
    goldfish_virtio_vsock_handle_host_to_guest(impl_);

    std::string transmitted;
    for (const CapturedPacket& pkt : packets_) {
        if (pkt.hdr.op == VIRTIO_VSOCK_OP_RW) {
            transmitted.append(pkt.payload);
        }
    }
    EXPECT_EQ(transmitted, kPayload);
}

TEST_F(VsockSnapshotFlagsTest, MultipleStreamsSavedAndRestored) {
    auto plug1 = std::make_shared<SnapshottablePlug>(/*tag=*/201);
    SocketPtr sock1 = Connect(kGuestPort, plug1);
    (void)CompleteHandshake(kGuestPort, /*guest_buf_alloc=*/65536);
    plug1->SetSocket(std::move(sock1));

    auto plug2 = std::make_shared<SnapshottablePlug>(/*tag=*/202);
    SocketPtr sock2 = Connect(kGuestPort + 1, plug2);
    (void)CompleteHandshake(kGuestPort + 1, /*guest_buf_alloc=*/65536);
    plug2->SetSocket(std::move(sock2));

    QEMUFile snapshot;
    ASSERT_EQ(goldfish_virtio_vsock_impl_save(impl_, &snapshot), 0);

    ASSERT_EQ(goldfish_virtio_vsock_impl_load(impl_, &snapshot), 0);
    EXPECT_EQ(SnapshottablePlug::RestoredPlugs().size(), 2U);
    EXPECT_TRUE(SnapshottablePlug::RestoredPlugs().count(201));
    EXPECT_TRUE(SnapshottablePlug::RestoredPlugs().count(202));
}

TEST_F(VsockSnapshotFlagsTest, CorruptedSnapshotRejectedGracefully) {
    auto plug = std::make_shared<SnapshottablePlug>(/*tag=*/301);
    SocketPtr sock = Connect(kGuestPort, plug);
    (void)CompleteHandshake(kGuestPort, /*guest_buf_alloc=*/65536);
    plug->SetSocket(std::move(sock));

    QEMUFile valid_snapshot;
    ASSERT_EQ(goldfish_virtio_vsock_impl_save(impl_, &valid_snapshot), 0);

    // Truncated at every possible prefix length must fail cleanly without crashing.
    for (size_t cut = 0; cut < valid_snapshot.buffer.size(); ++cut) {
        QEMUFile truncated;
        truncated.buffer = valid_snapshot.buffer.substr(0, cut);
        EXPECT_NE(goldfish_virtio_vsock_impl_load(impl_, &truncated), 0) << "cut=" << cut;
    }
}

class RecordingPlug : public IPlug {
  public:
    explicit RecordingPlug(std::string* received_out = nullptr) : received_out_(received_out) {}

    void OnConnect() override { connected_ = true; }

    SocketPtr OnUnplug() override { return nullptr; }

    bool OnReceive(const void* data, size_t size) override {
        if (received_out_ != nullptr) {
            received_out_->append(static_cast<const char*>(data), size);
        }
        return true;
    }

    bool SupportsLoadingFromSnapshot() const override { return false; }

  private:
    std::string* received_out_;
    bool connected_ = false;
};

class VsockTest : public ::testing::Test {
  protected:
    void SetUp() override {
        static const GoldfishVirtIOVSockDevAPI kApi = {
            .haveHostToGuestPackets = &VsockTest::OnHaveHostToGuestPackets,
            .sendPacketHostToGuest = &VsockTest::OnSendPacketHostToGuest,
            .haveHostToGuestEvents = &VsockTest::OnHaveHostToGuestEvents,
            .sendEventHostToGuest = &VsockTest::OnSendEventHostToGuest,
        };
        impl_ = goldfish_virtio_vsock_impl_realize(this, &kApi);
    }

    void TearDown() override {
        clear();
        goldfish_virtio_vsock_impl_unrealize(impl_);
    }

    SocketPtr ConnectStream(uint32_t guest_port, uint32_t guest_buf_alloc,
                            std::string* received_out = nullptr) {
        SocketPtr socket = Connect(guest_port, PlugPtr(new RecordingPlug(received_out)));
        uint32_t host_port = 0;
        for (auto it = packets_.rbegin(); it != packets_.rend(); ++it) {
            if (it->hdr.op == VIRTIO_VSOCK_OP_REQUEST && it->hdr.dst_port == guest_port) {
                host_port = it->hdr.src_port;
                break;
            }
        }
        EXPECT_NE(host_port, 0U);

        struct virtio_vsock_hdr resp = {
            .src_cid = kGuestCid,
            .dst_cid = kHostCid,
            .src_port = guest_port,
            .dst_port = host_port,
            .len = 0,
            .type = VIRTIO_VSOCK_TYPE_STREAM,
            .op = VIRTIO_VSOCK_OP_RESPONSE,
            .flags = 0,
            .buf_alloc = guest_buf_alloc,
            .fwd_cnt = 0,
        };
        goldfish_virtio_vsock_accept_guest_to_host_control(impl_, &resp);
        return socket;
    }

    void SendCreditUpdate(uint32_t guest_port, uint32_t host_port, uint32_t buf_alloc,
                          uint32_t fwd_cnt) {
        struct virtio_vsock_hdr credit = {
            .src_cid = kGuestCid,
            .dst_cid = kHostCid,
            .src_port = guest_port,
            .dst_port = host_port,
            .len = 0,
            .type = VIRTIO_VSOCK_TYPE_STREAM,
            .op = VIRTIO_VSOCK_OP_CREDIT_UPDATE,
            .flags = 0,
            .buf_alloc = buf_alloc,
            .fwd_cnt = fwd_cnt,
        };
        goldfish_virtio_vsock_accept_guest_to_host_control(impl_, &credit);
    }

    static void OnHaveHostToGuestPackets(void* opaque) {
        static_cast<VsockTest*>(opaque)->notify_count_++;
    }

    static VirtIOVSockSendResult OnSendPacketHostToGuest(void* opaque, struct virtio_vsock_hdr* hdr,
                                                         const void* data) {
        auto* self = static_cast<VsockTest*>(opaque);
        if (self->vq_bytes_allowance_ == 0 && hdr->len > 0) {
            return MakeVirtIOVSockSendResult(/*is_vq_full=*/true, /*need_notify=*/false, 0);
        }

        const size_t len = hdr->len;
        const size_t sent = std::min(len, self->vq_bytes_allowance_);
        const bool is_vq_full = sent < len;
        if (self->vq_bytes_allowance_ != SIZE_MAX) {
            self->vq_bytes_allowance_ -= sent;
        }

        CapturedPacket pkt;
        pkt.hdr = *hdr;
        pkt.hdr.len = static_cast<uint32_t>(sent);
        if (data != nullptr && sent > 0) {
            pkt.payload.assign(static_cast<const char*>(data), sent);
        }
        self->packets_.push_back(std::move(pkt));
        return MakeVirtIOVSockSendResult(is_vq_full, /*need_notify=*/true, sent);
    }

    static void OnHaveHostToGuestEvents(void* /*opaque*/) {}

    static VirtIOVSockSendResult OnSendEventHostToGuest(void* /*opaque*/,
                                                        const struct virtio_vsock_event* /*evt*/) {
        return MakeVirtIOVSockSendResult(false, false, 1);
    }

    void* impl_ = nullptr;
    size_t vq_bytes_allowance_ = SIZE_MAX;
    size_t notify_count_ = 0;
    std::vector<CapturedPacket> packets_;
};

TEST_F(VsockTest, AdvertisesOneMiBHostReceiveCreditWindow) {
    SocketPtr socket = Connect(kGuestPort, PlugPtr(new RecordingPlug()));
    ASSERT_FALSE(packets_.empty());
    EXPECT_EQ(packets_.front().hdr.buf_alloc, kExpectedHostBufAlloc);
    socket.reset();
}

TEST_F(VsockTest, IdleStreamsDoNotSendSpuriousCreditRequestsOnActiveStreamSend) {
    // Create an unaccepted stream whose guestBufAlloc is still 0.
    SocketPtr idle_socket = Connect(kGuestPort + 1, PlugPtr(new RecordingPlug()));
    ASSERT_FALSE(packets_.empty());
    const uint32_t idle_host_port = packets_.front().hdr.src_port;

    SocketPtr active_socket = ConnectStream(kGuestPort, /*guest_buf_alloc=*/65536);
    packets_.clear();

    const std::string payload = "adb-payload";
    active_socket->SendAsync(payload.data(), payload.size());

    for (const CapturedPacket& pkt : packets_) {
        EXPECT_NE(pkt.hdr.src_port, idle_host_port)
                << "Idle stream emitted spurious packet op=" << pkt.hdr.op;
    }

    active_socket.reset();
    idle_socket.reset();
}

TEST_F(VsockTest, CreditStarvationBuffersWithoutCreditRequestStormAndResumesOnCreditUpdate) {
    SocketPtr socket = ConnectStream(kGuestPort, /*guest_buf_alloc=*/8);
    const uint32_t host_port = packets_.front().hdr.src_port;
    packets_.clear();

    const std::string payload = "0123456789ABCDEF";
    socket->SendAsync(payload.data(), payload.size());

    // First 8 bytes should be sent via OP_RW, and the remaining 8 bytes must
    // be buffered without emitting VIRTIO_VSOCK_OP_CREDIT_REQUEST (which would
    // trigger an OP_CREDIT_REQUEST <-> OP_CREDIT_UPDATE ping-pong storm).
    ASSERT_EQ(packets_.size(), 1U);
    EXPECT_EQ(packets_[0].hdr.op, VIRTIO_VSOCK_OP_RW);
    EXPECT_EQ(packets_[0].payload, "01234567");

    // Repeated host_to_guest virtqueue drains while credit is 0 must not emit
    // spurious OP_CREDIT_REQUEST packets.
    EXPECT_EQ(goldfish_virtio_vsock_handle_host_to_guest(impl_), 0);
    EXPECT_EQ(packets_.size(), 1U);

    packets_.clear();
    SendCreditUpdate(kGuestPort, host_port, /*buf_alloc=*/8, /*fwd_cnt=*/8);
    EXPECT_NE(goldfish_virtio_vsock_handle_host_to_guest(impl_), 0);

    ASSERT_EQ(packets_.size(), 1U);
    EXPECT_EQ(packets_[0].hdr.op, VIRTIO_VSOCK_OP_RW);
    EXPECT_EQ(packets_[0].payload, "89ABCDEF");
    socket.reset();
}

class MockAsyncSocketServer : public goldfish::async::AsyncSocketServer {
  public:
    MOCK_METHOD(goldfish::network::Endpoint, GetEndpoint, (), (const, override));
    MOCK_METHOD(void, Close, (), (override));
    MOCK_METHOD(goldfish::async::EventLoop*, GetLoop, (), (const, override));
};

constexpr uint16_t kTestHostPort = 10525;

TEST(VsockPortFwdTest, ResolveForwarderEndpointsOrdersIpv4BeforeIpv6) {
    auto ipv4_endpoints = ResolveForwarderEndpoints("127.0.0.1:10525");
    ASSERT_TRUE(ipv4_endpoints.ok()) << ipv4_endpoints.status();
    ASSERT_FALSE(ipv4_endpoints->empty());
    EXPECT_TRUE(IsIpv4Endpoint(ipv4_endpoints->front()));
    EXPECT_EQ(goldfish::network::ToString(ipv4_endpoints->front()), "[127.0.0.1]:10525");

    auto ipv6_endpoints = ResolveForwarderEndpoints("[::1]:10525");
    if (ipv6_endpoints.ok()) {
        ASSERT_FALSE(ipv6_endpoints->empty());
        EXPECT_FALSE(IsIpv4Endpoint(ipv6_endpoints->front()));
        EXPECT_EQ(goldfish::network::ToString(ipv6_endpoints->front()), "[::1]:10525");
    }

    auto localhost_endpoints = ResolveForwarderEndpoints("localhost:10525");
    ASSERT_TRUE(localhost_endpoints.ok()) << localhost_endpoints.status();
    ASSERT_FALSE(localhost_endpoints->empty());
    EXPECT_TRUE(std::is_partitioned(localhost_endpoints->begin(), localhost_endpoints->end(),
                                    IsIpv4Endpoint));
}

TEST(VsockPortFwdTest, ResolveForwarderEndpointsRejectsInvalidHostname) {
    auto endpoints = ResolveForwarderEndpoints("invalid.host.that.does.not.exist.invalid:10525");
    EXPECT_FALSE(endpoints.ok());
    EXPECT_NE(endpoints.status().message().find("invalid.host.that.does.not.exist.invalid:10525"),
              std::string_view::npos);
}

TEST(VsockPortFwdTest, BindForwarderServerPrefersIpv4WhenAvailable) {
    using ::goldfish::async::testing::MockAsyncSocketFactory;
    using ::goldfish::network::Endpoint;
    using ::goldfish::network::kIPv4LoopbackAddress;
    using ::goldfish::network::kIPv6LoopbackAddress;
    using ::goldfish::network::ToEndpoint;
    using ::testing::_;
    using ::testing::Return;

    auto loop = goldfish::async::testing::TestEventLoop::Create();
    ::testing::NiceMock<MockAsyncSocketFactory> factory;

    const Endpoint ipv4_endpoint = ToEndpoint(kIPv4LoopbackAddress, kTestHostPort);
    const Endpoint ipv6_endpoint = ToEndpoint(kIPv6LoopbackAddress, kTestHostPort);
    const std::vector<Endpoint> endpoints = {ipv4_endpoint, ipv6_endpoint};

    auto mock_ipv4_server = std::make_shared<::testing::NiceMock<MockAsyncSocketServer>>();
    EXPECT_CALL(factory, CreateServer(loop.get(), ipv4_endpoint, _, _))
            .WillOnce(Return(mock_ipv4_server));
    EXPECT_CALL(factory, CreateServer(loop.get(), ipv6_endpoint, _, _)).Times(0);

    auto bound = BindForwarderServer(
            factory, loop.get(), endpoints, kGuestPort,
            [](std::shared_ptr<goldfish::async::AsyncSocket> /*socket*/) { return false; });
    ASSERT_TRUE(bound.ok()) << bound.status();
    EXPECT_EQ(bound->server, mock_ipv4_server);
    EXPECT_EQ(bound->endpoint, ipv4_endpoint);
}

TEST(VsockPortFwdTest, BindForwarderServerFallsBackToIpv6WhenIpv4Fails) {
    using ::goldfish::async::testing::MockAsyncSocketFactory;
    using ::goldfish::network::Endpoint;
    using ::goldfish::network::kIPv4LoopbackAddress;
    using ::goldfish::network::kIPv6LoopbackAddress;
    using ::goldfish::network::ToEndpoint;
    using ::testing::_;
    using ::testing::InSequence;
    using ::testing::Return;

    auto loop = goldfish::async::testing::TestEventLoop::Create();
    ::testing::NiceMock<MockAsyncSocketFactory> factory;

    const Endpoint ipv4_endpoint = ToEndpoint(kIPv4LoopbackAddress, kTestHostPort);
    const Endpoint ipv6_endpoint = ToEndpoint(kIPv6LoopbackAddress, kTestHostPort);
    const std::vector<Endpoint> endpoints = {ipv4_endpoint, ipv6_endpoint};

    auto mock_ipv6_server = std::make_shared<::testing::NiceMock<MockAsyncSocketServer>>();
    {
        InSequence seq;
        EXPECT_CALL(factory, CreateServer(loop.get(), ipv4_endpoint, _, _))
                .WillOnce(Return(nullptr));
        EXPECT_CALL(factory, CreateServer(loop.get(), ipv6_endpoint, _, _))
                .WillOnce(Return(mock_ipv6_server));
    }

    auto bound = BindForwarderServer(
            factory, loop.get(), endpoints, kGuestPort,
            [](std::shared_ptr<goldfish::async::AsyncSocket> /*socket*/) { return false; });
    ASSERT_TRUE(bound.ok()) << bound.status();
    EXPECT_EQ(bound->server, mock_ipv6_server);
    EXPECT_EQ(bound->endpoint, ipv6_endpoint);
}

TEST(VsockPortFwdTest, BindForwarderServerReturnsUnavailableWhenAllEndpointsFail) {
    using ::goldfish::async::testing::MockAsyncSocketFactory;
    using ::goldfish::network::Endpoint;
    using ::goldfish::network::kIPv4LoopbackAddress;
    using ::goldfish::network::kIPv6LoopbackAddress;
    using ::goldfish::network::ToEndpoint;
    using ::testing::_;
    using ::testing::Return;

    auto loop = goldfish::async::testing::TestEventLoop::Create();
    ::testing::NiceMock<MockAsyncSocketFactory> factory;

    const Endpoint ipv4_endpoint = ToEndpoint(kIPv4LoopbackAddress, kTestHostPort);
    const Endpoint ipv6_endpoint = ToEndpoint(kIPv6LoopbackAddress, kTestHostPort);
    const std::vector<Endpoint> endpoints = {ipv4_endpoint, ipv6_endpoint};

    EXPECT_CALL(factory, CreateServer(loop.get(), ipv4_endpoint, _, _)).WillOnce(Return(nullptr));
    EXPECT_CALL(factory, CreateServer(loop.get(), ipv6_endpoint, _, _)).WillOnce(Return(nullptr));

    auto bound = BindForwarderServer(
            factory, loop.get(), endpoints, kGuestPort,
            [](std::shared_ptr<goldfish::async::AsyncSocket> /*socket*/) { return false; });
    EXPECT_EQ(bound.status().code(), absl::StatusCode::kUnavailable);
    EXPECT_NE(bound.status().message().find("[127.0.0.1]:10525"), std::string_view::npos);
    EXPECT_NE(bound.status().message().find("[::1]:10525"), std::string_view::npos);
}

TEST(VsockPortFwdTest, BindForwarderServerRejectsEmptyEndpoints) {
    auto loop = goldfish::async::testing::TestEventLoop::Create();
    ::testing::NiceMock<goldfish::async::testing::MockAsyncSocketFactory> factory;

    auto bound = BindForwarderServer(
            factory, loop.get(), {}, kGuestPort,
            [](std::shared_ptr<goldfish::async::AsyncSocket> /*socket*/) { return false; });
    EXPECT_EQ(bound.status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace goldfish::vsock
