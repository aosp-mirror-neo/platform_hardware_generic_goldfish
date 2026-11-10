// Copyright 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "goldfish/net/virtio_wifi.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef BUILDING_QEMU_BINARY
#define BUILDING_QEMU_BINARY 1
#endif

#ifndef typeof
#define typeof __typeof__  // NOLINT(readability-identifier-naming)
#endif

#define _LINUX_VIRTIO_RING_H  // NOLINT(readability-identifier-naming)

// clang-format off
// IWYU pragma: begin_keep
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-user-defined-literal"
extern "C" {
#include "qemu/osdep.h"
#include "hw/virtio/virtio.h"
#include "migration/register.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/main-loop.h"
#include "standard-headers/linux/if_ether.h"

typedef struct VirtioPCIDeviceTypeInfo VirtioPCIDeviceTypeInfo;
}
#pragma clang diagnostic pop
// IWYU pragma: end_keep
// clang-format on

struct Error {
    std::string message;
};

struct QEMUBH {
    QEMUBHFunc* cb = nullptr;
    void* opaque = nullptr;
    bool scheduled = false;
    int cancel_count = 0;
};

struct VirtQueue {
    VirtIODevice* vdev = nullptr;
    int queue_size = 0;
    uint16_t queue_index = 0;
    VirtIOHandleOutput handle_output = nullptr;
    int ready = 1;
    int notification_enabled = 1;
    unsigned int drop_count = 0;
    unsigned int dropped_per_call = 0;
    int notify_count = 0;
};

namespace {

constexpr uint16_t kVirtioWifiLinkUp = 1;
constexpr size_t kVirtioWifiStatusOffset = sizeof(VirtIODevice) + ETH_ALEN;

uint16_t ReadWifiStatus(const VirtIODevice* vdev) {
    uint16_t status = 0;
    std::memcpy(&status, reinterpret_cast<const uint8_t*>(vdev) + kVirtioWifiStatusOffset,
                sizeof(status));
    return status;
}

struct RegisteredProperty {
    std::string type;
    ObjectPropertyAccessor* get = nullptr;
    ObjectPropertyAccessor* set = nullptr;
};

struct StubState {
    TypeInfo wifi_type_info{};
    bool wifi_type_registered = false;
    DeviceClass device_class{};
    VirtioDeviceClass virtio_device_class{};
    std::unordered_map<std::string, RegisteredProperty> properties;

    std::vector<VirtQueue*> queues;
    std::vector<QEMUBH*> bhs;

    NICState* nic = nullptr;
    NetClientState subqueue{};
    NetClientState peer{};
    bool has_peer = true;
    int find_net_clients_count = 1;

    std::string visitor_str;
    int32_t visitor_int32 = 0;

    std::vector<uint16_t> config_notify_statuses;
    int flush_queued_packets_count = 0;
    int flush_or_purge_count = 0;
};

StubState& GetStubState() {
    static auto* state = new StubState();
    return *state;
}

void ResetStubState() {
    StubState& s = GetStubState();
    for (VirtQueue* vq : s.queues) {
        delete vq;
    }
    s.queues.clear();
    for (QEMUBH* bh : s.bhs) {
        delete bh;
    }
    s.bhs.clear();
    if (s.nic != nullptr) {
        g_free(s.nic);
        s.nic = nullptr;
    }
    s.subqueue = NetClientState{};
    s.peer = NetClientState{};
    s.has_peer = true;
    s.find_net_clients_count = 1;
    s.visitor_str.clear();
    s.visitor_int32 = 0;
    s.config_notify_statuses.clear();
    s.flush_queued_packets_count = 0;
    s.flush_or_purge_count = 0;
}

}  // namespace

// NOLINTBEGIN(readability-identifier-naming)
extern "C" {

GOLDFISH_EXPORTED_VARIABLE const VMStateInfo virtio_vmstate_info = {
    .name = "virtio",
    .get = nullptr,
    .put = nullptr,
};

#ifdef _WIN32
extern const void* const __imp_virtio_vmstate_info = &virtio_vmstate_info;
#endif

TypeImpl* type_register_static(const TypeInfo* info) {
    StubState& s = GetStubState();
    if (info != nullptr && info->name != nullptr &&
        std::strcmp(info->name, "virtio-wifi-device") == 0) {
        s.wifi_type_info = *info;
        s.wifi_type_registered = true;
        if (info->class_init != nullptr) {
            info->class_init(reinterpret_cast<ObjectClass*>(&s.virtio_device_class), nullptr);
        }
    }
    return nullptr;
}

void virtio_pci_types_register(const VirtioPCIDeviceTypeInfo* /*t*/) {}

Object* object_dynamic_cast_assert(Object* obj, const char* /*typename_str*/, const char* /*file*/,
                                   int /*line*/, const char* /*func*/) {
    return obj;
}

ObjectClass* object_class_dynamic_cast_assert(ObjectClass* klass, const char* typename_str,
                                              const char* /*file*/, int /*line*/,
                                              const char* /*func*/) {
    StubState& s = GetStubState();
    if (typename_str != nullptr && std::strcmp(typename_str, TYPE_DEVICE) == 0) {
        return reinterpret_cast<ObjectClass*>(&s.device_class);
    }
    if (typename_str != nullptr && std::strcmp(typename_str, TYPE_VIRTIO_DEVICE) == 0) {
        return reinterpret_cast<ObjectClass*>(&s.virtio_device_class);
    }
    return klass;
}

ObjectProperty* object_class_property_add(ObjectClass* /*klass*/, const char* name,
                                          const char* type, ObjectPropertyAccessor* get,
                                          ObjectPropertyAccessor* set,
                                          ObjectPropertyRelease* /*release*/, void* /*opaque*/) {
    if (name != nullptr) {
        GetStubState().properties[name] = RegisteredProperty{
            .type = type != nullptr ? type : "",
            .get = get,
            .set = set,
        };
    }
    return nullptr;
}

const char* object_get_typename(const Object* /*obj*/) {
    return "virtio-wifi-device";
}

bool qdev_realize(DeviceState* /*dev*/, BusState* /*bus*/, Error** /*errp*/) {
    return true;
}

void virtio_instance_init_common(Object* /*proxy_obj*/, void* /*data*/, size_t /*vdev_size*/,
                                 const char* /*vdev_name*/) {}

bool visit_type_str(Visitor* /*v*/, const char* /*name*/, char** obj, Error** /*errp*/) {
    if (obj == nullptr) {
        return false;
    }
    *obj = g_strdup(GetStubState().visitor_str.c_str());
    return true;
}

bool visit_type_int32(Visitor* /*v*/, const char* /*name*/, int32_t* obj, Error** /*errp*/) {
    if (obj == nullptr) {
        return false;
    }
    *obj = GetStubState().visitor_int32;
    return true;
}

void error_setg_internal(Error** errp, const char* /*src*/, int /*line*/, const char* /*func*/,
                         const char* fmt, ...) {
    if (errp != nullptr && *errp == nullptr) {
        *errp = new Error{fmt != nullptr ? fmt : "error"};
    }
}

void virtio_init(VirtIODevice* vdev, uint16_t device_id, size_t /*config_size*/) {
    if (vdev != nullptr) {
        vdev->device_id = device_id;
    }
}

void virtio_cleanup(VirtIODevice* /*vdev*/) {}

VirtQueue* virtio_add_queue(VirtIODevice* vdev, int queue_size, VirtIOHandleOutput handle_output) {
    StubState& s = GetStubState();
    auto* vq = new VirtQueue();
    vq->vdev = vdev;
    vq->queue_size = queue_size;
    vq->queue_index = static_cast<uint16_t>(s.queues.size());
    vq->handle_output = handle_output;
    s.queues.push_back(vq);
    return vq;
}

void virtio_delete_queue(VirtQueue* /*vq*/) {}

void virtio_notify_config(VirtIODevice* vdev) {
    if (vdev != nullptr) {
        GetStubState().config_notify_statuses.push_back(ReadWifiStatus(vdev));
    }
}

void virtio_notify(VirtIODevice* /*vdev*/, VirtQueue* vq) {
    if (vq != nullptr) {
        vq->notify_count++;
    }
}

void virtio_error(VirtIODevice* /*vdev*/, const char* /*fmt*/, ...) {}

uint16_t virtio_get_queue_index(VirtQueue* vq) {
    return vq != nullptr ? vq->queue_index : 0;
}

int virtio_queue_ready(VirtQueue* vq) {
    return vq != nullptr ? vq->ready : 0;
}

void virtio_queue_set_notification(VirtQueue* vq, int enable) {
    if (vq != nullptr) {
        vq->notification_enabled = enable;
    }
}

unsigned int virtqueue_drop_all(VirtQueue* vq) {
    if (vq == nullptr) {
        return 0;
    }
    vq->drop_count++;
    return vq->dropped_per_call;
}

int virtqueue_get_avail_bytes(VirtQueue* /*vq*/, unsigned int* in_bytes, unsigned int* out_bytes,
                              unsigned /*max_in_bytes*/, unsigned /*max_out_bytes*/) {
    if (in_bytes != nullptr) {
        *in_bytes = 0;
    }
    if (out_bytes != nullptr) {
        *out_bytes = 0;
    }
    return 0;
}

void* virtqueue_pop(VirtQueue* /*vq*/, size_t /*sz*/) {
    return nullptr;
}

void virtqueue_push(VirtQueue* /*vq*/, const VirtQueueElement* /*elem*/, unsigned int /*len*/) {}

size_t iov_size(const struct iovec* /*iov*/, const unsigned int /*iov_cnt*/) {
    return 0;
}

size_t iov_from_buf_full(const struct iovec* /*iov*/, unsigned int /*iov_cnt*/, size_t /*offset*/,
                         const void* /*buf*/, size_t bytes) {
    return bytes;
}

QEMUBH* qemu_bh_new_full(QEMUBHFunc* cb, void* opaque, const char* /*name*/,
                         MemReentrancyGuard* /*reentrancy_guard*/) {
    auto* bh = new QEMUBH();
    bh->cb = cb;
    bh->opaque = opaque;
    GetStubState().bhs.push_back(bh);
    return bh;
}

void qemu_bh_schedule(QEMUBH* bh) {
    if (bh != nullptr) {
        bh->scheduled = true;
    }
}

void qemu_bh_cancel(QEMUBH* bh) {
    if (bh != nullptr) {
        bh->scheduled = false;
        bh->cancel_count++;
    }
}

void qemu_bh_delete(QEMUBH* /*bh*/) {}

NICState* qemu_new_nic(NetClientInfo* info, NICConf* conf, const char* /*model*/,
                       const char* /*name*/, MemReentrancyGuard* /*reentrancy_guard*/,
                       void* opaque) {
    StubState& s = GetStubState();
    s.nic = static_cast<NICState*>(g_malloc0(sizeof(NICState)));
    s.subqueue.info = info;
    s.subqueue.link_down = false;
    s.subqueue.queue_index = 0;
    s.subqueue.peer = s.has_peer ? &s.peer : nullptr;
    s.nic->ncs = &s.subqueue;
    s.nic->conf = conf;
    s.nic->opaque = opaque;
    return s.nic;
}

void qemu_del_nic(NICState* nic) {
    StubState& s = GetStubState();
    if (nic != nullptr && nic == s.nic) {
        g_free(s.nic);
        s.nic = nullptr;
    }
}

NetClientState* qemu_get_queue(NICState* nic) {
    return nic != nullptr ? nic->ncs : nullptr;
}

NetClientState* qemu_get_subqueue(NICState* nic, int /*queue_index*/) {
    return nic != nullptr ? nic->ncs : nullptr;
}

void* qemu_get_nic_opaque(NetClientState* nc) {
    StubState& s = GetStubState();
    if (nc == &s.subqueue && s.nic != nullptr) {
        return s.nic->opaque;
    }
    return nullptr;
}

void qemu_format_nic_info_str(NetClientState* /*nc*/, uint8_t /*macaddr*/[6]) {}

void qemu_flush_queued_packets(NetClientState* /*nc*/) {
    GetStubState().flush_queued_packets_count++;
}

void qemu_flush_or_purge_queued_packets(NetClientState* /*nc*/, bool /*purge*/) {
    GetStubState().flush_or_purge_count++;
}

ssize_t qemu_sendv_packet_async(NetClientState* /*nc*/, const struct iovec* /*iov*/, int /*iovcnt*/,
                                NetPacketSent* /*sent_cb*/) {
    return 0;
}

int qemu_find_net_clients_except(const char* /*id*/, NetClientState** ncs, NetClientDriver /*type*/,
                                 int /*max*/) {
    StubState& s = GetStubState();
    if (s.find_net_clients_count > 0 && ncs != nullptr) {
        s.peer.info = &s.subqueue.info[0];
        static NetClientInfo kPeerInfo = {};
        s.peer.info = &kPeerInfo;
        s.peer.peer = nullptr;
        ncs[0] = &s.peer;
    }
    return s.find_net_clients_count;
}

}  // extern "C"
// NOLINTEND(readability-identifier-naming)

namespace {

class VirtioWifiTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ResetStubState();
        StubState& s = GetStubState();
        if (!s.wifi_type_registered) {
            virtio_wifi_register_types();
        }
        ASSERT_TRUE(s.wifi_type_registered);
        ASSERT_NE(s.device_class.vmsd, nullptr);
        ASSERT_NE(s.device_class.vmsd->post_load, nullptr);

        device_storage_.assign(s.wifi_type_info.instance_size, 0);
        dev_ = reinterpret_cast<DeviceState*>(device_storage_.data());
        vdev_ = reinterpret_cast<VirtIODevice*>(dev_);
    }

    void TearDown() override {
        if (realized_) {
            GetStubState().virtio_device_class.unrealize(dev_);
            realized_ = false;
        }
        ResetStubState();
    }

    void RealizeDevice(const char* netdev_id = "wifi0", int32_t mac_prefix = 5554) {
        StubState& s = GetStubState();
        Error* err = nullptr;

        s.visitor_str = netdev_id;
        s.properties["netdev"].set(reinterpret_cast<Object*>(dev_), nullptr, "netdev", nullptr,
                                   &err);
        ASSERT_EQ(err, nullptr);

        s.visitor_int32 = mac_prefix;
        s.properties["mac_prefix"].set(reinterpret_cast<Object*>(dev_), nullptr, "mac_prefix",
                                       nullptr, &err);
        ASSERT_EQ(err, nullptr);

        s.virtio_device_class.realize(dev_, &err);
        ASSERT_EQ(err, nullptr);
        realized_ = true;

        vdev_->status = VIRTIO_CONFIG_S_DRIVER_OK;
        vdev_->vm_running = true;
    }

    std::vector<uint8_t> device_storage_;
    DeviceState* dev_ = nullptr;
    VirtIODevice* vdev_ = nullptr;
    bool realized_ = false;
};

TEST_F(VirtioWifiTest, PostLoadBouncesLinkStatusAndResetsQueuesWhenLinkUp) {
    RealizeDevice();
    StubState& s = GetStubState();
    ASSERT_EQ(s.queues.size(), 2U);
    VirtQueue* tx_vq = s.queues[0];
    VirtQueue* rx_vq = s.queues[1];
    ASSERT_EQ(s.bhs.size(), 1U);
    QEMUBH* tx_bh = s.bhs[0];

    // Simulate pre-snapshot runtime state where TX/RX notifications were
    // suppressed and a TX BH + stale TX descriptor were pending.
    tx_vq->notification_enabled = 0;
    tx_vq->dropped_per_call = 2;
    rx_vq->notification_enabled = 0;
    tx_bh->scheduled = true;

    ASSERT_EQ(ReadWifiStatus(vdev_), kVirtioWifiLinkUp);

    const int rc = s.device_class.vmsd->post_load(dev_, 1);
    EXPECT_EQ(rc, 0);

    // Link status must bounce down (0) and back up (VIRTIO_WIFI_LINK_UP),
    // notifying guest config on each transition.
    const std::vector<uint16_t> expected_statuses = {0, kVirtioWifiLinkUp};
    EXPECT_EQ(s.config_notify_statuses, expected_statuses);
    EXPECT_EQ(ReadWifiStatus(vdev_), kVirtioWifiLinkUp);
    EXPECT_FALSE(s.subqueue.link_down);

    // Stale TX descriptors must be dropped, BH cancelled, and both TX/RX
    // virtqueue notifications re-enabled.
    EXPECT_FALSE(tx_bh->scheduled);
    EXPECT_GE(tx_bh->cancel_count, 1);
    EXPECT_EQ(tx_vq->drop_count, 1U);
    EXPECT_EQ(tx_vq->notify_count, 1);
    EXPECT_EQ(tx_vq->notification_enabled, 1);
    EXPECT_EQ(rx_vq->notification_enabled, 1);
    EXPECT_GE(s.flush_queued_packets_count, 1);
}

TEST_F(VirtioWifiTest, PostLoadKeepsLinkDownWhenBackendPeerIsLinkDown) {
    RealizeDevice();
    StubState& s = GetStubState();
    s.peer.link_down = true;

    const int rc = s.device_class.vmsd->post_load(dev_, 1);
    EXPECT_EQ(rc, 0);

    const std::vector<uint16_t> expected_statuses = {0};
    EXPECT_EQ(s.config_notify_statuses, expected_statuses);
    EXPECT_EQ(ReadWifiStatus(vdev_), 0U);
    EXPECT_TRUE(s.subqueue.link_down);
}

TEST_F(VirtioWifiTest, RealizeAndPostLoadWithUtf8NetdevAndDeviceId) {
    static constexpr char kUtf8Id[] = "wifi-テスト-🚀";
    dev_->id = const_cast<char*>(kUtf8Id);
    RealizeDevice(kUtf8Id, /*mac_prefix=*/5556);

    StubState& s = GetStubState();
    EXPECT_EQ(s.nic->conf->macaddr.a[4], (5556 >> 8) & 0xff);
    EXPECT_EQ(s.nic->conf->macaddr.a[5], 5556 & 0xff);

    const int rc = s.device_class.vmsd->post_load(dev_, 1);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(ReadWifiStatus(vdev_), kVirtioWifiLinkUp);
}

}  // namespace
