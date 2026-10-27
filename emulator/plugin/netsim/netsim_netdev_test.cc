// Copyright 2026 The Android Open Source Project
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

#include "goldfish/netsim/netsim_netdev.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/status/status.h"

#include "emulator/plugin/netsim/packet_transport.h"

extern "C" {
// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/main-loop.h"
#include "qom/object.h"
// IWYU pragma: end_keep
// clang-format on
}

#undef send

struct Error {
    std::string message;
};

namespace {

struct FakeQemuEnv {
    const TypeInfo* type_info = nullptr;
    DeviceClass device_class{};
    std::vector<Object*> deletable_objects;
    std::unordered_map<std::string, NetClientState*> netdevs;
    std::vector<std::unique_ptr<std::byte[]>> allocations;
    std::vector<std::unique_ptr<Error>> errors;
    bool bql_is_locked = false;
    int transport_create_count = 0;
    int transport_destroy_count = 0;

    void Reset() {
        deletable_objects.clear();
        netdevs.clear();
        allocations.clear();
        errors.clear();
        bql_is_locked = false;
        transport_create_count = 0;
        transport_destroy_count = 0;
    }
};

FakeQemuEnv& GetFakeQemuEnv() {
    static auto* env = new FakeQemuEnv();
    return *env;
}

class FakePacketTransport : public goldfish::netsim::PacketTransport {
  public:
    explicit FakePacketTransport(RecvCallback recv_cb) : recv_cb_(std::move(recv_cb)) {
        ++GetFakeQemuEnv().transport_create_count;
    }

    ~FakePacketTransport() override { ++GetFakeQemuEnv().transport_destroy_count; }

    absl::Status initialize(const goldfish::netsim::ChipConfig& /*chip*/) override {
        return absl::OkStatus();
    }

    void send(goldfish::netsim::Packet /*packet*/) override {}

    void next_recv() override {}

  private:
    RecvCallback recv_cb_;
};

std::unique_ptr<goldfish::netsim::PacketTransport> CreateFakePacketTransport(
        goldfish::netsim::PacketTransport::RecvCallback recv_cb) {
    return std::make_unique<FakePacketTransport>(std::move(recv_cb));
}

}  // namespace

extern "C" {

Type type_register_static(const TypeInfo* info) {
    auto& env = GetFakeQemuEnv();
    env.type_info = info;
    env.device_class = DeviceClass{};
    if (info != nullptr && info->class_init != nullptr) {
        info->class_init(reinterpret_cast<ObjectClass*>(&env.device_class), info->class_data);
    }
    return nullptr;
}

ObjectProperty* object_class_property_add(ObjectClass* /*klass*/, const char* /*name*/,
                                          const char* /*type*/, ObjectPropertyAccessor* /*get*/,
                                          ObjectPropertyAccessor* /*set*/,
                                          ObjectPropertyRelease* /*release*/, void* /*opaque*/) {
    return nullptr;
}

ObjectClass* object_class_dynamic_cast_assert(ObjectClass* klass, const char* /*type_name*/,
                                              const char* /*file*/, int /*line*/,
                                              const char* /*func*/) {
    return klass;
}

Object* object_dynamic_cast_assert(Object* obj, const char* /*type_name*/, const char* /*file*/,
                                   int /*line*/, const char* /*func*/) {
    return obj;
}

char* object_get_canonical_path(const Object* /*obj*/) {
    return nullptr;
}

Object* object_resolve_path_type(const char* /*path*/, const char* /*type_name*/,
                                 bool* /*ambiguous*/) {
    return nullptr;
}

Object* object_resolve_type_unambiguous(const char* /*type_name*/, Error** /*errp*/) {
    return nullptr;
}

bool visit_type_str(Visitor* /*v*/, const char* /*name*/, char** obj, Error** /*errp*/) {
    if (obj != nullptr) {
        *obj = nullptr;
    }
    return false;
}

bool visit_type_uint32(Visitor* /*v*/, const char* /*name*/, uint32_t* obj, Error** /*errp*/) {
    if (obj != nullptr) {
        *obj = 0;
    }
    return false;
}

int add_deletable_object(Object* obj) {
    GetFakeQemuEnv().deletable_objects.push_back(obj);
    return 0;
}

void error_setg_internal(Error** errp, const char* /*src*/, int /*line*/, const char* /*func*/,
                         const char* fmt, ...) {
    if (errp == nullptr) {
        return;
    }
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    auto err = std::make_unique<Error>(Error{.message = buffer});
    *errp = err.get();
    GetFakeQemuEnv().errors.push_back(std::move(err));
}

NetClientState* qemu_find_netdev(const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    auto& netdevs = GetFakeQemuEnv().netdevs;
    const auto it = netdevs.find(id);
    return it == netdevs.end() ? nullptr : it->second;
}

NetClientState* qemu_new_net_client(NetClientInfo* info, NetClientState* peer, const char* model,
                                    const char* name) {
    auto& env = GetFakeQemuEnv();
    const size_t alloc_size = std::max(sizeof(NetClientState), info->size);
    auto raw = std::make_unique<std::byte[]>(alloc_size);
    std::memset(raw.get(), 0, alloc_size);

    auto* nc = reinterpret_cast<NetClientState*>(raw.get());
    nc->info = info;
    nc->peer = peer;
    nc->model = const_cast<char*>(model);
    nc->name = const_cast<char*>(name);

    env.allocations.push_back(std::move(raw));
    if (name != nullptr) {
        env.netdevs[name] = nc;
    }
    return nc;
}

void qemu_del_net_client(NetClientState* nc) {
    if (nc == nullptr) {
        return;
    }
    if (nc->info != nullptr && nc->info->cleanup != nullptr) {
        nc->info->cleanup(nc);
    }
    if (nc->name != nullptr) {
        GetFakeQemuEnv().netdevs.erase(nc->name);
    }
}

ssize_t qemu_send_packet_async(NetClientState* /*nc*/, const uint8_t* /*buf*/, int size,
                               NetPacketSent* /*sent_cb*/) {
    return size;
}

// NOLINTNEXTLINE(readability-identifier-naming)
size_t iov_size(const struct iovec* iov, const unsigned int iov_cnt) {
    size_t total = 0;
    for (unsigned int i = 0; i < iov_cnt; ++i) {
        total += iov[i].iov_len;
    }
    return total;
}

// NOLINTNEXTLINE(readability-identifier-naming)
size_t iov_to_buf_full(const struct iovec* iov, const unsigned int iov_cnt, size_t offset,
                       void* buf, size_t bytes) {
    auto* out = static_cast<uint8_t*>(buf);
    size_t copied = 0;
    size_t skipped = 0;
    for (unsigned int i = 0; i < iov_cnt && copied < bytes; ++i) {
        const size_t len = iov[i].iov_len;
        if (skipped + len <= offset) {
            skipped += len;
            continue;
        }
        const size_t inner_offset = offset > skipped ? offset - skipped : 0;
        const size_t to_copy = std::min(bytes - copied, len - inner_offset);
        std::memcpy(out + copied, static_cast<const uint8_t*>(iov[i].iov_base) + inner_offset,
                    to_copy);
        copied += to_copy;
        skipped += len;
    }
    return copied;
}

bool bql_locked(void) {
    return GetFakeQemuEnv().bql_is_locked;
}

void bql_lock_impl(const char* /*file*/, int /*line*/) {
    GetFakeQemuEnv().bql_is_locked = true;
}

void bql_unlock(void) {
    GetFakeQemuEnv().bql_is_locked = false;
}

}  // extern "C"

namespace goldfish::netsim {
namespace {

using ::testing::_;

class FakeDeviceInstance {
  public:
    explicit FakeDeviceInstance(std::string id) : id_(std::move(id)) {
        const auto& env = GetFakeQemuEnv();
        const size_t instance_bytes =
                std::max(sizeof(DeviceState), env.type_info ? env.type_info->instance_size : 0);
        storage_.resize(instance_bytes, std::byte{0});
        GetDevice()->id = id_.data();
    }

    DeviceState* GetDevice() { return reinterpret_cast<DeviceState*>(storage_.data()); }

  private:
    std::string id_;
    alignas(std::max_align_t) std::vector<std::byte> storage_;
};

class NetsimNetdevTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() { netsim_netdev_register_types(); }

    void SetUp() override {
        GetFakeQemuEnv().Reset();
        SetNetsimNetdevPacketTransportFactoryForTesting(&CreateFakePacketTransport);
        ASSERT_NE(GetFakeQemuEnv().type_info, nullptr);
        ASSERT_NE(GetFakeQemuEnv().device_class.realize, nullptr);
        ASSERT_NE(GetFakeQemuEnv().device_class.unrealize, nullptr);
    }

    void TearDown() override { SetNetsimNetdevPacketTransportFactoryForTesting(nullptr); }
};

TEST_F(NetsimNetdevTest, CleanShutdownAfterNetCleanupDoesNotLogError) {
    FakeDeviceInstance eth0("eth0");
    FakeDeviceInstance wifi("wifi");

    Error* err = nullptr;
    GetFakeQemuEnv().device_class.realize(eth0.GetDevice(), &err);
    ASSERT_EQ(err, nullptr);
    GetFakeQemuEnv().device_class.realize(wifi.GetDevice(), &err);
    ASSERT_EQ(err, nullptr);

    ASSERT_EQ(GetFakeQemuEnv().transport_create_count, 2);
    ASSERT_NE(qemu_find_netdev("eth0"), nullptr);
    ASSERT_NE(qemu_find_netdev("wifi"), nullptr);

    // Simulate QEMU's qemu_cleanup() teardown sequence: net_cleanup() destroys
    // netdevs before deletable_cleanup() unrealizes deletable QOM devices.
    qemu_del_net_client(qemu_find_netdev("eth0"));
    qemu_del_net_client(qemu_find_netdev("wifi"));
    EXPECT_EQ(GetFakeQemuEnv().transport_destroy_count, 2);
    EXPECT_EQ(qemu_find_netdev("eth0"), nullptr);
    EXPECT_EQ(qemu_find_netdev("wifi"), nullptr);

    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kError, _, _)).Times(0);
    log.StartCapturingLogs();

    GetFakeQemuEnv().device_class.unrealize(eth0.GetDevice());
    GetFakeQemuEnv().device_class.unrealize(wifi.GetDevice());

    log.StopCapturingLogs();
    EXPECT_EQ(GetFakeQemuEnv().transport_destroy_count, 2);
}

TEST_F(NetsimNetdevTest, UnrealizeDeletesRemainingNetdevOnUnplug) {
    FakeDeviceInstance dev("wifi-テスト");

    Error* err = nullptr;
    GetFakeQemuEnv().device_class.realize(dev.GetDevice(), &err);
    ASSERT_EQ(err, nullptr);
    ASSERT_NE(qemu_find_netdev("wifi-テスト"), nullptr);
    EXPECT_EQ(GetFakeQemuEnv().transport_destroy_count, 0);

    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kError, _, _)).Times(0);
    log.StartCapturingLogs();

    GetFakeQemuEnv().device_class.unrealize(dev.GetDevice());

    log.StopCapturingLogs();
    EXPECT_EQ(qemu_find_netdev("wifi-テスト"), nullptr);
    EXPECT_EQ(GetFakeQemuEnv().transport_destroy_count, 1);
}

}  // namespace
}  // namespace goldfish::netsim
