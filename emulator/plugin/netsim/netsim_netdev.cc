// Copyright 2025 The Android Open Source Project
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

#include <sys/types.h>

#include <memory>
#include <string>

#include "absl/log/check.h"
#include "absl/log/log.h"

extern "C" {
// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
// IWYU pragma: end_keep
// clang-format on

// qemu/iov.h can't be included because of void* arithmetic
size_t iov_size(const struct iovec* iov, const unsigned int iov_cnt);
size_t iov_to_buf_full(const struct iovec* iov, const unsigned int iov_cnt, size_t offset,
                       void* buf, size_t bytes);
}

#undef send

#include "emulator/plugin/netsim/packet_transport.h"
#include "goldfish/network/generic_netlink_message.h"

#define __packed
typedef int8_t s8;
typedef uint8_t u8;
typedef uint16_t u16;
#include "standard-headers/linux/mac80211_hwsim.h"

namespace goldfish::netsim {

namespace {

PacketTransportFactory create_transport = nullptr;

struct NetsimNetdev {
    DeviceClass parent_class;
    ChipKind chip_kind{ChipKind::kWifi};
};

#define TYPE_NETSIM_NETDEV "netsim-netdev"
#define NETSIM_NETDEV(obj) OBJECT_CHECK(NetsimNetdev, (obj), TYPE_NETSIM_NETDEV)
#define NETSIM_NETDEV_DEVICE_GET_CLASS(obj) OBJECT_GET_CLASS(NetsimNetdev, obj, TYPE_NETSIM_NETDEV)

struct NetsimState {
    std::unique_ptr<PacketTransport> transport;
    std::unique_ptr<std::vector<uint8_t>> async_tx;
};

struct NetsimNicState {
    // Note C struct so no c/d-tor.
    NetClientState nc;
    NetsimState* netsim;
    bool is_wifi;
};

void netsim_netdev_send_completed(NetClientState* nc, ssize_t len) {
    VLOG(2) << "NETSIM: async completed";
    auto* s = reinterpret_cast<NetsimNicState*>(nc);
    // Clean-up the saved buffer.
    s->netsim->async_tx.reset();

    if (s->netsim->transport) {
        s->netsim->transport->next_recv();
    }
}

bool netsim_netdev_send(NetClientState* nc, Packet& packet) {
    // From netsim to guest.
    VLOG(2) << "NETSIM: send (netsim -> guest)";

    auto* s = reinterpret_cast<NetsimNicState*>(nc);
    if (s->netsim->async_tx) {
        LOG(DFATAL) << "Netsim recv: async_tx not empty, dropping packet - this is a bug and "
                       "network performance may be affected";
        return false;
    }

    if (qemu_send_packet_async(nc, packet.data(), packet.size(), netsim_netdev_send_completed) ==
        0) {
        VLOG(2) << "NETSIM: storing async_tx";
        // Keep the buffer alive until the send completes.
        s->netsim->async_tx = std::make_unique<Packet>(std::move(packet));
        return false;
    } else {
        return true;
    }
}

ssize_t netsim_netdev_receive_iov(NetClientState* nc, const struct iovec* iov, int iovcnt) {
    // From guest to netsim.
    VLOG(2) << "NETSIM: receive_iov (netsim <- guest)";
    if (iovcnt <= 0) {
        return -1;
    }
    auto* s = reinterpret_cast<NetsimNicState*>(nc);

    const size_t total_len = iov_size(iov, iovcnt);
    if (total_len == 0) {
        return -1;
    }
    if (total_len > NET_BUFSIZE) {
        LOG(WARNING) << "Packet length exceeds maximum allowed packet size";
        return -1;
    }

    Packet packet(total_len);
    iov_to_buf_full(iov, iovcnt, 0, packet.data(), total_len);

    if (s->is_wifi) {
        // Filter out spurious garbage data from the guest.
        if (total_len <
            sizeof(goldfish::network::nlmsghdr) + sizeof(goldfish::network::genlmsghdr)) {
            // Too small for GenericNetlinkMessage.
            return total_len;
        }
        const goldfish::network::GenericNetlinkMessage msg(packet.data(), total_len);
        if (msg.genericNetlinkHeader()->cmd != HWSIM_CMD_FRAME) {
            VLOG(1) << "Not sending junk frame";
            return total_len;
        }
        packet.resize(msg.dataLen());
    }
    s->netsim->transport->send(std::move(packet));

    // TODO(whollins): Should we try to detect netsimd connection drop and set link down?

    return total_len;
}

void reset_transport(NetsimState* netsim) {
    bool locked = bql_locked();
    if (locked) {
        bql_unlock();
    }
    // Reset transport first to cancel gRPC call and wait for OnDone notification.
    // This ensures no gRPC callbacks are executing concurrently during teardown.
    netsim->transport.reset();
    if (locked) {
        bql_lock();
    }
}

void netsim_netdev_link_status_changed(NetClientState* nc) {
    VLOG(1) << "NETSIM: link status changed: " << !nc->link_down;
}

void netsim_netdev_cleanup(NetClientState* nc) {
    auto* s = reinterpret_cast<NetsimNicState*>(nc);
    if (!s || !s->netsim) {
        return;
    }

    reset_transport(s->netsim);

    delete s->netsim;
    s->netsim = nullptr;
}

NetClientInfo netsim_netdev_nic_info = {
    // We could add our own value in qapi/net.json but
    // it doesn't seem to be necessary.
    // .type = NET_CLIENT_DRIVER_NETSIM,
    .type = NET_CLIENT_DRIVER_NONE,
    .size = sizeof(NetsimNicState),
    .receive_iov = netsim_netdev_receive_iov,
    .cleanup = netsim_netdev_cleanup,
    .link_status_changed = netsim_netdev_link_status_changed,
};

void netsim_netdev_realize(DeviceState* dev, Error** errp) {
    add_deletable_object(OBJECT(dev));
    if (dev->id == nullptr) {
        error_setg(errp, "id attribute must be set");
        return;
    }

    VLOG(1) << "Realizing netsim netdev: " << dev->id;

    NetsimNetdev* netsim_netdev = NETSIM_NETDEV(dev);

    NetClientState* nc;
    nc = qemu_find_netdev(dev->id);
    if (nc != nullptr) {
        error_setg(errp, "netdev with id already exists: %s", dev->id);
        return;
    }

    NetClientState* peer = nullptr;
    nc = qemu_new_net_client(&netsim_netdev_nic_info, peer, "netsim", dev->id);
    nc->is_netdev = true;

    auto* s = reinterpret_cast<NetsimNicState*>(nc);

    s->netsim = new NetsimState;
    const PacketTransportFactory factory =
            create_transport != nullptr ? create_transport : &CreatePacketTransport;
    s->netsim->transport = factory([nc](Packet& packet) {
        bql_lock();
        bool delivered = netsim_netdev_send(nc, packet);
        bql_unlock();
        return delivered;
    });
    s->is_wifi = netsim_netdev->chip_kind == ChipKind::kWifi;

    ChipConfig chip{
        .kind = netsim_netdev->chip_kind,
    };
    if (auto status = s->netsim->transport->initialize(chip); !status.ok()) {
        error_setg(errp, "failed to initialize netsim transport %s: %s", dev->id,
                   status.ToString().c_str());
        reset_transport(s->netsim);
        delete s->netsim;
        s->netsim = nullptr;
        qemu_del_net_client(nc);
        return;
    }
}

void netsim_netdev_unrealize(DeviceState* dev) {
    VLOG(1) << "Unrealizing netsim netdev: " << dev->id;

    NetClientState* nc = qemu_find_netdev(dev->id);
    if (nc == nullptr) {
        // During qemu_cleanup(), net_cleanup() destroys netdevs before
        // deletable_cleanup() unrealizes deletable QOM devices.
        VLOG(1) << "Netdev already cleaned up: " << dev->id;
        return;
    }
    qemu_del_net_client(nc);
}

void netsim_netdev_set_mode(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) {
    auto* netdev = NETSIM_NETDEV(obj);
    char* mode;
    if (!visit_type_str(v, name, &mode, errp)) {
        return;
    }
    VLOG(1) << "netsim-netdev: mode = " << mode;
    std::string s_mode(mode);
    if (s_mode == "wifi") {
        netdev->chip_kind = ChipKind::kWifi;
    } else if (s_mode == "ethernet") {
        netdev->chip_kind = ChipKind::kEthernet;
    } else if (s_mode == "cellular") {
        netdev->chip_kind = ChipKind::kCellularData;
    }
}

void netsim_netdev_class_init(ObjectClass* oc, const void* data) {
    object_class_property_add(oc, "mode", "str", nullptr, netsim_netdev_set_mode, nullptr, nullptr);

    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = netsim_netdev_realize;
    dc->unrealize = netsim_netdev_unrealize;
}

const TypeInfo netsim_netdev_type_info = {
    .name = TYPE_NETSIM_NETDEV,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(NetsimNetdev),
    .class_init = netsim_netdev_class_init,
};

}  // namespace

void netsim_netdev_register_types(void) {
    type_register_static(&goldfish::netsim::netsim_netdev_type_info);
}

void SetNetsimNetdevPacketTransportFactoryForTesting(PacketTransportFactory factory) {
    create_transport = factory;
}

}  // namespace goldfish::netsim
