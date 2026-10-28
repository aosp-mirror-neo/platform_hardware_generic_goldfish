/* Copyright 2025 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

// Note that this code was originally inspired by Qemu hw/net/virtio-net.h.

#include "goldfish/net/virtio_wifi.h"

#include <stdbool.h>

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "migration/register.h"
#include "standard-headers/linux/if_ether.h"
#include "standard-headers/linux/virtio_ids.h"
#include "qemu/iov.h"
#include "qapi/visitor.h"
// IWYU pragma: end_keep
// clang-format on

// NOLINTBEGIN(readability-identifier-naming)

#define __packed
typedef int8_t s8;
typedef uint8_t u8;
typedef uint16_t u16;
#include "android/base/logging/abseil_log_bridge.h"
#include "standard-headers/linux/mac80211_hwsim.h"

/* Limit the number of packets that can be sent via a single flush
 * of the TX queue.  This gives us a guaranteed exit condition and
 * ensures fairness in the io path.  256 conveniently matches the
 * length of the TX queue and shows a good balance of performance
 * and latency. */
static const size_t kTXBurst = 256;
// The mac80211_hwsim driver only supports one TX/RX pair.
static const size_t kQueueSize = 1;

static const size_t VIRTIO_WIFI_RX_QUEUE_DEFAULT_SIZE = 256;
static const size_t VIRTIO_WIFI_TX_QUEUE_DEFAULT_SIZE = 256;

const uint16_t VIRTIO_WIFI_LINK_UP = 1;

static const uint8_t kMacAddr[] = {0x02, 0x15, 0xb2, 0x00, 0x00, 0x00};

typedef struct VirtIOWifi VirtIOWifi;
typedef struct VirtIOWifiQueue {
    VirtQueue* rx;
    VirtQueue* tx;
    QEMUBH* tx_bh;
    uint32_t tx_waiting;
    struct {
        VirtQueueElement* elem;
    } async_tx;
    VirtIOWifi* wifi_dev;
} VirtIOWifiQueue;

struct VirtIOWifi {
    VirtIODevice parent_obj;
    uint8_t mac[ETH_ALEN];
    uint16_t status;
    int32_t tx_burst;
    VirtIOWifiQueue* vqs;
    NICState* nic;
    NICConf nic_conf;

    bool netdev_set, mac_prefix_set;
};

#define TYPE_VIRTIO_WIFI "virtio-wifi-device"
DECLARE_INSTANCE_CHECKER(VirtIOWifi, VIRTIO_WIFI, TYPE_VIRTIO_WIFI);

static bool virtio_wifi_started(VirtIOWifi* wifi, uint8_t status) {
    VirtIODevice* vdev = VIRTIO_DEVICE(wifi);
    return (status & VIRTIO_CONFIG_S_DRIVER_OK) && (wifi->status & VIRTIO_WIFI_LINK_UP) &&
           vdev->vm_running;
}

static void virtio_wifi_drop_tx_queue_data(VirtIODevice* vdev, VirtIOWifiQueue* q) {
    unsigned int dropped = virtqueue_drop_all(q->tx);
    if (dropped) {
        virtio_notify(vdev, q->tx);
    }
}

static int virtio_wifi_set_status(VirtIODevice* vdev, uint8_t status) {
    ALOGV(2, "Set status: 0x%x", status);
    VirtIOWifi* wifi = VIRTIO_WIFI(vdev);
    bool link_down = (wifi->status & VIRTIO_WIFI_LINK_UP) == 0;

    for (size_t i = 0; i < kQueueSize; i++) {
        NetClientState* ncs = qemu_get_subqueue(wifi->nic, i);
        VirtIOWifiQueue* q = &wifi->vqs[i];

        ncs->link_down = link_down;

        uint8_t queue_status = status;
        bool queue_started = virtio_wifi_started(wifi, status);

        if (queue_started) {
            qemu_flush_queued_packets(ncs);
        }

        if (!q->tx_waiting) {
            continue;
        }

        if (queue_started) {
            ALOGV(1, "Set status: up");
            qemu_bh_schedule(q->tx_bh);
        } else {
            ALOGV(1, "Set status: down");
            qemu_bh_cancel(q->tx_bh);
            if ((wifi->status & VIRTIO_WIFI_LINK_UP) == 0 &&
                (queue_status & VIRTIO_CONFIG_S_DRIVER_OK && vdev->vm_running)) {
                q->tx_waiting = 0;
                virtio_queue_set_notification(q->tx, 1);
                virtio_wifi_drop_tx_queue_data(vdev, q);
            }
        }
    }
    return 0;
}

// set virtio-wifi link status according to netclientstate
static void virtio_wifi_nic_link_status_changed(NetClientState* nc) {
    VirtIOWifi* wifi = qemu_get_nic_opaque(nc);
    VirtIODevice* vdev = VIRTIO_DEVICE(wifi);
    uint16_t old_status = wifi->status;
    ALOGV(2, "Set link status: %d", !nc->link_down);
    if (nc->link_down) {
        wifi->status &= ~VIRTIO_WIFI_LINK_UP;
    } else {
        wifi->status |= VIRTIO_WIFI_LINK_UP;
    }

    if (wifi->status != old_status) {
        ALOGV(1, "Link status changed: %d", !nc->link_down);
        virtio_notify_config(vdev);
    }

    virtio_wifi_set_status(vdev, vdev->status);
}

static int virtio_wifi_post_load(void* opaque, int version_id) {
    (void)version_id;
    VirtIOWifi* wifi = VIRTIO_WIFI(opaque);
    VirtIODevice* vdev = VIRTIO_DEVICE(wifi);

    for (size_t i = 0; i < kQueueSize; i++) {
        VirtIOWifiQueue* q = &wifi->vqs[i];
        qemu_bh_cancel(q->tx_bh);
        q->tx_waiting = 0;
        if (q->async_tx.elem) {
            g_free(q->async_tx.elem);
            q->async_tx.elem = NULL;
        }
        if (virtio_queue_ready(q->tx)) {
            virtio_wifi_drop_tx_queue_data(vdev, q);
            virtio_queue_set_notification(q->tx, 1);
        }
        if (virtio_queue_ready(q->rx)) {
            virtio_queue_set_notification(q->rx, 1);
        }
    }

    NetClientState* nc = qemu_get_queue(wifi->nic);
    bool backend_link_up = (nc == NULL || !nc->link_down) &&
                           (nc == NULL || nc->peer == NULL || !nc->peer->link_down);

    if (wifi->status & VIRTIO_WIFI_LINK_UP) {
        wifi->status &= ~VIRTIO_WIFI_LINK_UP;
        virtio_notify_config(vdev);
        virtio_wifi_set_status(vdev, vdev->status);
    }

    if (backend_link_up) {
        wifi->status |= VIRTIO_WIFI_LINK_UP;
        virtio_notify_config(vdev);
        virtio_wifi_set_status(vdev, vdev->status);
    }

    return 0;
}

static VMStateField virtio_wifi_vmstate_fields[2];
static const VMStateDescription virtio_wifi_vmstate = {
    .name = TYPE_VIRTIO_WIFI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = virtio_wifi_post_load,
    .fields = virtio_wifi_vmstate_fields,
};

static bool virtio_wifi_nic_can_rx(NetClientState* nc) {
    VirtIOWifi* wifi = qemu_get_nic_opaque(nc);
    VirtIOWifiQueue* q = &wifi->vqs[nc->queue_index];
    VirtIODevice* vdev = VIRTIO_DEVICE(wifi);
    if (!vdev->vm_running) {
        ALOGV(2, "NIC can receive: false");
        return false;
    }
    if (!virtio_queue_ready(q->rx) || !(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        ALOGV(2, "NIC can receive: false");
        return false;
    }
    return true;
}

static int virtio_wifi_has_rx_buffers(VirtIOWifiQueue* q, int size_needed) {
    // Alternative to virtio_queue_enable_notification_and_check that only calls functions that
    // already hold the RCU lock.
    unsigned int size_available;

    // virtqueue_get_avail_bytes safely acquires the RCU lock internally.
    virtqueue_get_avail_bytes(q->rx, &size_available, NULL, size_needed, 0);

    if (size_available < size_needed) {
        // Enable notification to wake us up when the guest adds buffers
        virtio_queue_set_notification(q->rx, 1);

        virtqueue_get_avail_bytes(q->rx, &size_available, NULL, size_needed, 0);
        if (size_available < size_needed) {
            return 0;  // Still not enough, expect to be notified.
        }
    }

    // We have buffers, disable notification while processing
    virtio_queue_set_notification(q->rx, 0);

    return 1;
}

// Receive packets from the nic and push them onto the virtio rx queue
static ssize_t virtio_wifi_nic_rx(NetClientState* nc, const uint8_t* buf, size_t size) {
    // Note that we should only call virtqueue functions that lock/unlock rcu themselves and
    // don't assume it is held by the caller.
    ALOGV(2, "NIC RX (-> guest)");

    if (!virtio_wifi_nic_can_rx(nc)) {
        return -1;
    }

    VirtIOWifi* wifi = qemu_get_nic_opaque(nc);
    VirtIODevice* vdev = VIRTIO_DEVICE(wifi);

    VirtIOWifiQueue* q = &wifi->vqs[nc->queue_index];

    if (!virtio_wifi_has_rx_buffers(q, size)) {
        return 0;
    }

    VirtQueueElement* elem = virtqueue_pop(q->rx, sizeof(VirtQueueElement));
    if (!elem) {
        virtio_error(vdev, "virtio-wifi unexpected empty queue");
        return -1;
    }

    const uint32_t elemCapacity = iov_size(elem->in_sg, elem->in_num);
    if (elemCapacity < size) {
        ALOGW("VirtIO WiFi: received a very large (%d bytes) buffer, truncating to %d bytes.", size,
              elemCapacity);
        size = elemCapacity;
    }

    iov_from_buf(elem->in_sg, elem->in_num, 0, buf, size);
    virtqueue_push(q->rx, elem, size);
    g_free(elem);

    // virtqueue_flush(q->rx, 1);
    virtio_notify(vdev, q->rx);

    return size;
}

// The virtio rx queue is ready, flush any packets waiting in the NIC queue.
static void virtio_wifi_q_handle_rx(VirtIODevice* vdev, VirtQueue* vq) {
    ALOGV(2, "virtio rx q empty buffers available");
    VirtIOWifi* wifi = VIRTIO_WIFI(vdev);
    // The NIC only has one queue.
    int queue_index = 0;
    qemu_flush_queued_packets(qemu_get_subqueue(wifi->nic, queue_index));
}

static void virtio_wifi_nic_tx_complete(NetClientState* nc, ssize_t len) {
    ALOGV(1, "NIC async TX completed");
    VirtIOWifi* wifi = qemu_get_nic_opaque(nc);
    VirtIOWifiQueue* q = &wifi->vqs[nc->queue_index];
    VirtIODevice* vdev = VIRTIO_DEVICE(wifi);

    virtqueue_push(q->tx, q->async_tx.elem, 0);
    virtio_notify(vdev, q->tx);

    g_free(q->async_tx.elem);
    q->async_tx.elem = NULL;

    // There might be more buffers waiting - don't enable notifications, just reschedule.
    q->tx_waiting = 1;
    qemu_bh_schedule(q->tx_bh);
}

// Take packets off the virtio tx queue and push them to the NIC
static bool virtio_wifi_flush_tx_to_nic(VirtIOWifiQueue* q) {
    ALOGV(2, "NIC TX (<- guest)");
    VirtIOWifi* wifi = q->wifi_dev;
    VirtIODevice* vdev = VIRTIO_DEVICE(wifi);
    VirtQueueElement* elem;
    size_t num_packets = 0;

    // Only one NIC queue
    int queue_index = 0;

    if (!(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        return false;
    }

    if (q->async_tx.elem) {
        return false;
    }

    for (;;) {
        ssize_t ret;

        elem = virtqueue_pop(q->tx, sizeof(VirtQueueElement));
        if (!elem) {
            // Double check available buffers.
            virtio_queue_set_notification(q->tx, 1);
            elem = virtqueue_pop(q->tx, sizeof(VirtQueueElement));
            if (!elem) {
                return false;
            }
            virtio_queue_set_notification(q->tx, 0);
        }

        ret = qemu_sendv_packet_async(qemu_get_subqueue(wifi->nic, queue_index), elem->out_sg,
                                      elem->out_num, virtio_wifi_nic_tx_complete);
        if (ret == 0) {
            q->async_tx.elem = elem;
            return false;
        }

        virtqueue_push(q->tx, elem, 0);
        virtio_notify(vdev, q->tx);
        g_free(elem);

        if (++num_packets >= wifi->tx_burst) {
            // Request an immediate reschedule in case the burst wasn't completed.
            return true;
        }
    }

    // unreachable
    return true;
}

static void virtio_wifi_tx_bh(void* opaque) {
    ALOGV(2, "TX BH running");
    VirtIOWifiQueue* q = opaque;
    VirtIOWifi* wifi = q->wifi_dev;
    VirtIODevice* vdev = VIRTIO_DEVICE(wifi);

    // This happens when device was stopped but BH wasn't.
    if (!vdev->vm_running) {
        // Make sure tx waiting is set, so we'll run when restarted.
        assert(q->tx_waiting);
        return;
    }

    q->tx_waiting = 0;

    if (unlikely(!(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK))) {
        return;
    }

    if (virtio_wifi_flush_tx_to_nic(q)) {
        // Reschedule immediately.
        q->tx_waiting = 1;
        ALOGV(2, "TX BH re-scheduling");
        qemu_bh_schedule(q->tx_bh);
    }
}

// The virtio tx queue has packets to be sent, schedule flush to nic.
static void virtio_wifi_q_handle_tx(VirtIODevice* vdev, VirtQueue* vq) {
    ALOGV(2, "virtio tx q data buffers available");
    VirtIOWifi* wifi = VIRTIO_WIFI(vdev);
    VirtIOWifiQueue* q = &wifi->vqs[virtio_get_queue_index(vq) / 2];
    if (unlikely(wifi->status & VIRTIO_WIFI_LINK_UP) == 0) {
        virtio_wifi_drop_tx_queue_data(vdev, q);
        return;
    }
    if (unlikely(q->tx_waiting)) {
        return;
    }
    q->tx_waiting = 1;
    // This happens when device was stopped but VCPU wasn't.
    if (!vdev->vm_running) {
        return;
    }
    ALOGV(2, "TX BH scheduling");
    virtio_queue_set_notification(vq, 0);
    qemu_bh_schedule(q->tx_bh);
}

static NetClientInfo virtio_wifi_nic_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = virtio_wifi_nic_can_rx,
    .receive = virtio_wifi_nic_rx,
    .link_status_changed = virtio_wifi_nic_link_status_changed,
};

static void virtio_wifi_device_realize(DeviceState* dev, Error** errp) {
    VirtIODevice* vdev = VIRTIO_DEVICE(dev);
    VirtIOWifi* wifi = VIRTIO_WIFI(dev);

    if (!wifi->netdev_set) {
        error_setg(errp, "virtio-wifi netdev property not set");
        return;
    }
    if (!wifi->mac_prefix_set) {
        error_setg(errp, "virtio-wifi mac_prefix property not set");
        return;
    }

    virtio_init(vdev, VIRTIO_ID_MAC80211_HWSIM, 0);

    wifi->vqs = g_new0(VirtIOWifiQueue, kQueueSize);
    for (size_t index = 0; index < kQueueSize; index++) {
        VirtIOWifiQueue* q = &wifi->vqs[index];

        q->wifi_dev = wifi;
        q->tx_waiting = 0;
        q->async_tx.elem = NULL;

        // Per
        // http://cs/h/android/kernel/superproject/+/common-android-mainline:common/drivers/net/wireless/virtual/mac80211_hwsim.h
        // TX must be queue 0
        q->tx = virtio_add_queue(vdev, VIRTIO_WIFI_TX_QUEUE_DEFAULT_SIZE, virtio_wifi_q_handle_tx);
        q->tx_bh = qemu_bh_new_guarded(virtio_wifi_tx_bh, q, &DEVICE(vdev)->mem_reentrancy_guard);

        // RX must be queue 1
        q->rx = virtio_add_queue(vdev, VIRTIO_WIFI_RX_QUEUE_DEFAULT_SIZE, virtio_wifi_q_handle_rx);
    }

    wifi->status = VIRTIO_WIFI_LINK_UP;

    // qemu_macaddr_default_if_unset(&wifi->nic_conf.macaddr);
    memcpy(wifi->nic_conf.macaddr.a, wifi->mac, sizeof(wifi->mac));

    wifi->nic_conf.peers.queues = kQueueSize;
    wifi->nic =
            qemu_new_nic(&virtio_wifi_nic_info, &wifi->nic_conf, object_get_typename(OBJECT(dev)),
                         dev->id, &dev->mem_reentrancy_guard, wifi);

    qemu_format_nic_info_str(qemu_get_queue(wifi->nic), wifi->nic_conf.macaddr.a);

    wifi->tx_burst = kTXBurst;
}

static void virtio_wifi_device_unrealize(DeviceState* dev) {
    VirtIODevice* vdev = VIRTIO_DEVICE(dev);
    VirtIOWifi* wifi = VIRTIO_WIFI(dev);
    for (size_t index = 0; index < kQueueSize; index++) {
        VirtIOWifiQueue* q = &wifi->vqs[index];
        virtio_delete_queue(q->rx);
        virtio_delete_queue(q->tx);
        qemu_bh_delete(q->tx_bh);
        q->tx_bh = NULL;
    }
    g_free(wifi->vqs);

    qemu_del_nic(wifi->nic);
    virtio_cleanup(vdev);
}

static uint64_t virtio_wifi_get_features(VirtIODevice* vdev, uint64_t features, Error** errp) {
    return 0;
}

static void virtio_wifi_set_features(VirtIODevice* vdev, uint64_t features) {}

static uint64_t virtio_wifi_bad_features(VirtIODevice* vdev) {
    return 0;
}

static void virtio_wifi_instance_init(Object* obj) {
    // VirtIOWifi* wifi = VIRTIO_WIFI(obj);
}

static void flush_or_purge_queued_packets(NetClientState* nc) {
    if (!nc->peer) {
        return;
    }

    qemu_flush_or_purge_queued_packets(nc->peer, true);

    VirtIOWifi* wifi = qemu_get_nic_opaque(nc);
    assert(!wifi->vqs[nc->queue_index].async_tx.elem);
}

static void virtio_wifi_queue_reset(VirtIODevice* vdev, uint32_t queue_index) {
    ALOGV(1, "queue reset");
    VirtIOWifi* wifi = VIRTIO_WIFI(vdev);
    NetClientState* nc;

    // Only one NIC Queue
    queue_index = 0;
    nc = qemu_get_subqueue(wifi->nic, queue_index);

    if (!nc->peer) {
        return;
    }

    flush_or_purge_queued_packets(nc);
}

static void virtio_wifi_reset(VirtIODevice* vdev) {
    ALOGV(1, "device reset");
    VirtIOWifi* wifi = VIRTIO_WIFI(vdev);

    // Only one NIC Queue
    uint32_t queue_index = 0;
    // Flush any async TX
    flush_or_purge_queued_packets(qemu_get_subqueue(wifi->nic, queue_index));
}

// copied from set_netdev() in qdev-properties-system.c
static void virtio_wifi_set_netdev(Object* obj, Visitor* v, const char* name, void* opaque,
                                   Error** errp) {
    char* netdev;
    if (!visit_type_str(v, name, &netdev, errp)) {
        return;
    }

    ALOGV(1, "netdev = %s", netdev);

    VirtIOWifi* wifi = VIRTIO_WIFI(obj);
    NICPeers* peers_ptr = &wifi->nic_conf.peers;
    NetClientState** ncs = peers_ptr->ncs;

    NetClientState* peers[MAX_QUEUE_NUM];
    int queues, i = 0;
    queues = qemu_find_net_clients_except(netdev, peers, NET_CLIENT_DRIVER_NIC, MAX_QUEUE_NUM);
    if (queues == 0) {
        error_setg(errp, "backend '%s' has 0 queue", netdev);
        goto out;
    }

    if (queues > MAX_QUEUE_NUM) {
        error_setg(errp, "queues of backend '%s'(%d) exceeds QEMU limitation(%d)", netdev, queues,
                   MAX_QUEUE_NUM);
        goto out;
    }

    for (i = 0; i < queues; i++) {
        if (peers[i]->peer) {
            error_setg(errp, "queues of backend '%s'(%d) already has a peer", netdev, i);
            goto out;
        }

        if (peers[i]->info->check_peer_type) {
            if (!peers[i]->info->check_peer_type(peers[i], obj->klass, errp)) {
                goto out;
            }
        }

        ncs[i] = peers[i];
        ncs[i]->queue_index = i;
    }

    peers_ptr->queues = queues;

    wifi->netdev_set = true;

out:
    g_free(netdev);
}

static void virtio_wifi_set_mac_prefix(Object* obj, Visitor* v, const char* name, void* opaque,
                                       Error** errp) {
    // Default is 5554 (emulator serial number)
    int32_t mac_prefix;
    if (!visit_type_int32(v, name, &mac_prefix, errp)) {
        return;
    }

    ALOGV(1, "mac_prefix = %d", mac_prefix);

    VirtIOWifi* wifi = VIRTIO_WIFI(obj);

    memcpy(wifi->mac, kMacAddr, ETH_ALEN);
    wifi->mac[4] = (mac_prefix >> 8) & 0xff;
    wifi->mac[5] = mac_prefix & 0xff;

    wifi->mac_prefix_set = true;
}

static void virtio_wifi_class_init(ObjectClass* klass, const void* data) {
    DeviceClass* dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->vmsd = &virtio_wifi_vmstate;

    object_class_property_add(klass, "netdev", "str", NULL, virtio_wifi_set_netdev, NULL, NULL);
    object_class_property_add(klass, "mac_prefix", "int32", NULL, &virtio_wifi_set_mac_prefix, NULL,
                              NULL);

    VirtioDeviceClass* vdc = VIRTIO_DEVICE_CLASS(klass);
    vdc->realize = virtio_wifi_device_realize;
    vdc->unrealize = virtio_wifi_device_unrealize;
    vdc->get_features = virtio_wifi_get_features;
    vdc->set_features = virtio_wifi_set_features;
    vdc->bad_features = virtio_wifi_bad_features;
    vdc->reset = virtio_wifi_reset;
    vdc->queue_reset = virtio_wifi_queue_reset;
    vdc->set_status = virtio_wifi_set_status;
}

static const TypeInfo virtio_wifi_info = {
    .name = TYPE_VIRTIO_WIFI,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOWifi),
    .instance_init = virtio_wifi_instance_init,
    .class_init = virtio_wifi_class_init,
};

// PCI stuff below here
typedef struct VirtIOWifiPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOWifi vdev;
} VirtIOWifiPCI;

#define TYPE_VIRTIO_WIFI_PCI "virtio-wifi-pci"
DECLARE_INSTANCE_CHECKER(VirtIOWifiPCI, VIRTIO_WIFI_PCI, TYPE_VIRTIO_WIFI_PCI);

static void virtio_wifi_pci_realize(VirtIOPCIProxy* vpci_dev, Error** errp) {
    vpci_dev->nvectors = kQueueSize * 2 + 1;
    vpci_dev->flags |= VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT;

    VirtIOWifiPCI* dev = VIRTIO_WIFI_PCI(vpci_dev);
    DeviceState* vdev = DEVICE(&dev->vdev);

    // This is a "legacy" device.
    vpci_dev->disable_legacy = ON_OFF_AUTO_OFF;
    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_wifi_pci_class_init(ObjectClass* klass, const void* data) {
    DeviceClass* dc = DEVICE_CLASS(klass);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);

    PCIDeviceClass* k = PCI_DEVICE_CLASS(klass);
    k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    k->device_id = PCI_DEVICE_ID_VIRTIO_MAC80211_WLAN;
    k->revision = VIRTIO_PCI_ABI_VERSION;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;

    VirtioPCIClass* vpciklass = VIRTIO_PCI_CLASS(klass);
    vpciklass->realize = virtio_wifi_pci_realize;
}

static void virtio_wifi_pci_instance_init(Object* obj) {
    VirtIOWifiPCI* dev = VIRTIO_WIFI_PCI(obj);
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev), TYPE_VIRTIO_WIFI);
}

static const VirtioPCIDeviceTypeInfo virtio_wifi_pci_info = {
    .generic_name = TYPE_VIRTIO_WIFI_PCI,
    .instance_size = sizeof(VirtIOWifiPCI),
    .instance_init = virtio_wifi_pci_instance_init,
    .class_init = virtio_wifi_pci_class_init,
};

void virtio_wifi_register_types(void) {
    /*
     * b/445476051: VMStateField must be initialized at runtime because it refers
     * (through VMSTATE_xyz) to QEMU symbols (in a different binary) and on Windows
     * their addresses are runtime values.
     */
    virtio_wifi_vmstate_fields[0] = (VMStateField)VMSTATE_VIRTIO_DEVICE;
    virtio_wifi_vmstate_fields[1] = (VMStateField)VMSTATE_END_OF_LIST();

    type_register_static(&virtio_wifi_info);
    virtio_pci_types_register(&virtio_wifi_pci_info);
}

// NOLINTEND(readability-identifier-naming)
