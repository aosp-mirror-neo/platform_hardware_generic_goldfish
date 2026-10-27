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

// clang-format off
#include "qemu/osdep.h"
// clang-format on

#include "libqos/libqos-malloc.h"
#include "libqos/malloc-pc.h"
#include "libqos/pci-pc.h"
#include "libqos/virtio-pci.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "standard-headers/linux/virtio_config.h"
#include "standard-headers/linux/virtio_ring.h"
#include "standard-headers/linux/virtio_snd.h"

#define VIRTIO_SND_PCI_SLOT 0x09
#define VIRTIO_QUEUE_TIMEOUT_US (5 * 1000 * 1000)

/* 16 kHz mono S16: 160 frames = 320 bytes per 10 ms period. */
#define GUEST_PERIOD_FRAMES 160U
#define GUEST_PERIOD_BYTES (GUEST_PERIOD_FRAMES * sizeof(int16_t))
#define GUEST_BUFFER_BYTES (GUEST_PERIOD_BYTES * 4U)
#define PERIOD_NS (10LL * 1000LL * 1000LL)
#define SILENCE_MAX_ABS 16

typedef struct VirtioSndTestCtx {
    QTestState* qts;
    QGuestAllocator alloc;
    QPCIBus* pcibus;
    QVirtioPCIDevice* vpci;
    QVirtQueue* ctrlq;
    QVirtQueue* txq;
    QVirtQueue* rxq;
} VirtioSndTestCtx;

static void virtio_snd_ctrl_cmd(VirtioSndTestCtx* ctx, const void* req, size_t req_len, void* resp,
                                size_t resp_len) {
    uint64_t req_addr = guest_alloc(&ctx->alloc, req_len);
    uint64_t resp_addr = guest_alloc(&ctx->alloc, resp_len);
    uint32_t head;
    uint32_t used_len = 0;

    qtest_memwrite(ctx->qts, req_addr, req, req_len);
    qtest_memset(ctx->qts, resp_addr, 0, resp_len);

    head = qvirtqueue_add(ctx->qts, ctx->ctrlq, req_addr, req_len,
                          /*write=*/false, /*next=*/true);
    qvirtqueue_add(ctx->qts, ctx->ctrlq, resp_addr, resp_len,
                   /*write=*/true, /*next=*/false);
    qvirtqueue_kick(ctx->qts, &ctx->vpci->vdev, ctx->ctrlq, head);

    qvirtio_wait_used_elem(ctx->qts, &ctx->vpci->vdev, ctx->ctrlq, head, &used_len,
                           VIRTIO_QUEUE_TIMEOUT_US);
    g_assert_cmpuint(used_len, ==, resp_len);
    qtest_memread(ctx->qts, resp_addr, resp, resp_len);

    guest_free(&ctx->alloc, resp_addr);
    guest_free(&ctx->alloc, req_addr);
}

static uint32_t virtio_snd_find_stream(VirtioSndTestCtx* ctx, uint8_t direction) {
    struct virtio_snd_query_info req = {
        .hdr.code = cpu_to_le32(VIRTIO_SND_R_PCM_INFO),
        .start_id = cpu_to_le32(0),
        .count = cpu_to_le32(2),
        .size = cpu_to_le32(sizeof(struct virtio_snd_pcm_info)),
    };
    struct {
        struct virtio_snd_hdr hdr;
        struct virtio_snd_pcm_info infos[2];
    } QEMU_PACKED resp;

    virtio_snd_ctrl_cmd(ctx, &req, sizeof(req), &resp, sizeof(resp));
    g_assert_cmphex(le32_to_cpu(resp.hdr.code), ==, VIRTIO_SND_S_OK);

    for (uint32_t i = 0; i < 2; ++i) {
        if (resp.infos[i].direction == direction) {
            return i;
        }
    }
    g_assert_not_reached();
}

static void virtio_snd_simple_pcm_cmd(VirtioSndTestCtx* ctx, uint32_t code, uint32_t stream_id) {
    struct virtio_snd_pcm_hdr req = {
        .hdr.code = cpu_to_le32(code),
        .stream_id = cpu_to_le32(stream_id),
    };
    struct virtio_snd_hdr resp = {0};

    virtio_snd_ctrl_cmd(ctx, &req, sizeof(req), &resp, sizeof(resp));
    g_assert_cmphex(le32_to_cpu(resp.code), ==, VIRTIO_SND_S_OK);
}

static void virtio_snd_playback_one_period(VirtioSndTestCtx* ctx, uint32_t stream_id) {
    struct virtio_snd_pcm_xfer xfer_hdr = {
        .stream_id = cpu_to_le32(stream_id),
    };
    int16_t tx_samples[GUEST_PERIOD_FRAMES];
    const size_t out_total_bytes = sizeof(xfer_hdr) + GUEST_PERIOD_BYTES;
    uint64_t out_addr = guest_alloc(&ctx->alloc, out_total_bytes);
    uint64_t in_addr = guest_alloc(&ctx->alloc, sizeof(struct virtio_snd_pcm_status));
    struct virtio_snd_pcm_status status_resp = {0};
    uint32_t head;
    uint32_t used_len = 0;

    for (size_t i = 0; i < GUEST_PERIOD_FRAMES; ++i) {
        tx_samples[i] = (int16_t)cpu_to_le16(4096U);
    }

    qtest_memwrite(ctx->qts, out_addr, &xfer_hdr, sizeof(xfer_hdr));
    qtest_memwrite(ctx->qts, out_addr + sizeof(xfer_hdr), tx_samples, GUEST_PERIOD_BYTES);
    qtest_memset(ctx->qts, in_addr, 0x5a, sizeof(status_resp));

    head = qvirtqueue_add(ctx->qts, ctx->txq, out_addr, out_total_bytes,
                          /*write=*/false, /*next=*/true);
    qvirtqueue_add(ctx->qts, ctx->txq, in_addr, sizeof(status_resp),
                   /*write=*/true, /*next=*/false);
    qvirtqueue_kick(ctx->qts, &ctx->vpci->vdev, ctx->txq, head);

    qtest_clock_step(ctx->qts, PERIOD_NS);

    qvirtio_wait_used_elem(ctx->qts, &ctx->vpci->vdev, ctx->txq, head, &used_len,
                           VIRTIO_QUEUE_TIMEOUT_US);
    g_assert_cmpuint(used_len, ==, sizeof(status_resp));

    qtest_memread(ctx->qts, in_addr, &status_resp, sizeof(status_resp));
    g_assert_cmphex(le32_to_cpu(status_resp.status), ==, VIRTIO_SND_S_OK);

    guest_free(&ctx->alloc, in_addr);
    guest_free(&ctx->alloc, out_addr);
}

static void virtio_snd_capture_one_period(VirtioSndTestCtx* ctx, uint32_t stream_id,
                                          int16_t* out_samples) {
    struct virtio_snd_pcm_xfer xfer_hdr = {
        .stream_id = cpu_to_le32(stream_id),
    };
    const size_t in_total_bytes = GUEST_PERIOD_BYTES + sizeof(struct virtio_snd_pcm_status);
    uint64_t out_addr = guest_alloc(&ctx->alloc, sizeof(xfer_hdr));
    uint64_t in_addr = guest_alloc(&ctx->alloc, in_total_bytes);
    struct virtio_snd_pcm_status status_resp = {0};
    uint32_t head;
    uint32_t used_len = 0;

    qtest_memwrite(ctx->qts, out_addr, &xfer_hdr, sizeof(xfer_hdr));
    qtest_memset(ctx->qts, in_addr, 0x5a, in_total_bytes);

    head = qvirtqueue_add(ctx->qts, ctx->rxq, out_addr, sizeof(xfer_hdr),
                          /*write=*/false, /*next=*/true);
    qvirtqueue_add(ctx->qts, ctx->rxq, in_addr, in_total_bytes,
                   /*write=*/true, /*next=*/false);
    qvirtqueue_kick(ctx->qts, &ctx->vpci->vdev, ctx->rxq, head);

    qtest_clock_step(ctx->qts, PERIOD_NS);

    qvirtio_wait_used_elem(ctx->qts, &ctx->vpci->vdev, ctx->rxq, head, &used_len,
                           VIRTIO_QUEUE_TIMEOUT_US);
    g_assert_cmpuint(used_len, ==, in_total_bytes);

    qtest_memread(ctx->qts, in_addr, out_samples, GUEST_PERIOD_BYTES);
    qtest_memread(ctx->qts, in_addr + GUEST_PERIOD_BYTES, &status_resp, sizeof(status_resp));
    g_assert_cmphex(le32_to_cpu(status_resp.status), ==, VIRTIO_SND_S_OK);

    for (size_t i = 0; i < GUEST_PERIOD_FRAMES; ++i) {
        out_samples[i] = (int16_t)le16_to_cpu((uint16_t)out_samples[i]);
    }

    guest_free(&ctx->alloc, in_addr);
    guest_free(&ctx->alloc, out_addr);
}

static bool period_is_silent(const int16_t* samples) {
    for (size_t i = 0; i < GUEST_PERIOD_FRAMES; ++i) {
        if (samples[i] < -SILENCE_MAX_ABS || samples[i] > SILENCE_MAX_ABS) {
            return false;
        }
    }
    return true;
}

static void virtio_snd_test_init(VirtioSndTestCtx* ctx) {
    ctx->qts = qtest_init(
            "-M pc -nodefaults "
            "-audiodev none,id=hostaudiodev,"
            "out.mixing-engine=on,out.fixed-settings=on,"
            "out.frequency=48000,out.format=s16,out.channels=2,"
            "in.mixing-engine=on,in.fixed-settings=on,"
            "in.frequency=48000,in.format=s16,in.channels=1 "
            "-device goldfish-audio-mux,"
            "host-audiodev=hostaudiodev,audiodev-id=mainaudiodev "
            "-device virtio-sound-pci,audiodev=mainaudiodev,addr=09.0");

    pc_alloc_init(&ctx->alloc, ctx->qts, ALLOC_NO_FLAGS);
    ctx->pcibus = qpci_new_pc(ctx->qts, &ctx->alloc);

    QPCIAddress addr = {
        .devfn = QPCI_DEVFN(VIRTIO_SND_PCI_SLOT, 0),
    };
    ctx->vpci = virtio_pci_new(ctx->pcibus, &addr);
    g_assert_nonnull(ctx->vpci);
    qvirtio_pci_device_enable(ctx->vpci);
    qvirtio_start_device(&ctx->vpci->vdev);

    uint64_t features = qvirtio_get_features(&ctx->vpci->vdev);
    features &= ~(QVIRTIO_F_BAD_FEATURE | (1ull << VIRTIO_RING_F_INDIRECT_DESC) |
                  (1ull << VIRTIO_RING_F_EVENT_IDX));
    qvirtio_set_features(&ctx->vpci->vdev, features);

    ctx->ctrlq = qvirtqueue_setup(&ctx->vpci->vdev, &ctx->alloc, VIRTIO_SND_VQ_CONTROL);
    ctx->txq = qvirtqueue_setup(&ctx->vpci->vdev, &ctx->alloc, VIRTIO_SND_VQ_TX);
    ctx->rxq = qvirtqueue_setup(&ctx->vpci->vdev, &ctx->alloc, VIRTIO_SND_VQ_RX);
    qvirtio_set_driver_ok(&ctx->vpci->vdev);
}

static void virtio_snd_test_teardown(VirtioSndTestCtx* ctx) {
    qvirtqueue_cleanup(ctx->vpci->vdev.bus, ctx->rxq, &ctx->alloc);
    qvirtqueue_cleanup(ctx->vpci->vdev.bus, ctx->txq, &ctx->alloc);
    qvirtqueue_cleanup(ctx->vpci->vdev.bus, ctx->ctrlq, &ctx->alloc);
    qvirtio_pci_device_disable(ctx->vpci);
    g_free(ctx->vpci->pdev);
    g_free(ctx->vpci);
    qpci_free_pc(ctx->pcibus);
    alloc_destroy(&ctx->alloc);
    qtest_quit(ctx->qts);
}

static void virtio_snd_configure_16k_mono_s16(VirtioSndTestCtx* ctx, uint32_t stream_id) {
    struct virtio_snd_pcm_set_params set_params = {
        .hdr =
                {
                    .hdr.code = cpu_to_le32(VIRTIO_SND_R_PCM_SET_PARAMS),
                    .stream_id = cpu_to_le32(stream_id),
                },
        .buffer_bytes = cpu_to_le32(GUEST_BUFFER_BYTES),
        .period_bytes = cpu_to_le32(GUEST_PERIOD_BYTES),
        .features = 0,
        .channels = 1,
        .format = VIRTIO_SND_PCM_FMT_S16,
        .rate = VIRTIO_SND_PCM_RATE_16000,
    };
    struct virtio_snd_hdr ctrl_resp = {0};

    virtio_snd_ctrl_cmd(ctx, &set_params, sizeof(set_params), &ctrl_resp, sizeof(ctrl_resp));
    g_assert_cmphex(le32_to_cpu(ctrl_resp.code), ==, VIRTIO_SND_S_OK);
}

static void test_passthrough_playback(void) {
    VirtioSndTestCtx ctx = {0};
    uint32_t output_stream_id;

    virtio_snd_test_init(&ctx);
    output_stream_id = virtio_snd_find_stream(&ctx, VIRTIO_SND_D_OUTPUT);
    virtio_snd_configure_16k_mono_s16(&ctx, output_stream_id);

    virtio_snd_simple_pcm_cmd(&ctx, VIRTIO_SND_R_PCM_PREPARE, output_stream_id);
    virtio_snd_simple_pcm_cmd(&ctx, VIRTIO_SND_R_PCM_START, output_stream_id);

    for (int i = 0; i < 2; ++i) {
        virtio_snd_playback_one_period(&ctx, output_stream_id);
    }

    virtio_snd_simple_pcm_cmd(&ctx, VIRTIO_SND_R_PCM_STOP, output_stream_id);
    virtio_snd_simple_pcm_cmd(&ctx, VIRTIO_SND_R_PCM_RELEASE, output_stream_id);
    virtio_snd_test_teardown(&ctx);
}

static void test_passthrough_capture(void) {
    VirtioSndTestCtx ctx = {0};
    int16_t samples[GUEST_PERIOD_FRAMES];
    uint32_t input_stream_id;

    virtio_snd_test_init(&ctx);
    input_stream_id = virtio_snd_find_stream(&ctx, VIRTIO_SND_D_INPUT);
    virtio_snd_configure_16k_mono_s16(&ctx, input_stream_id);

    virtio_snd_simple_pcm_cmd(&ctx, VIRTIO_SND_R_PCM_PREPARE, input_stream_id);
    virtio_snd_simple_pcm_cmd(&ctx, VIRTIO_SND_R_PCM_START, input_stream_id);

    for (int i = 0; i < 3; ++i) {
        virtio_snd_capture_one_period(&ctx, input_stream_id, samples);
        g_assert_true(period_is_silent(samples));
    }

    virtio_snd_simple_pcm_cmd(&ctx, VIRTIO_SND_R_PCM_STOP, input_stream_id);
    virtio_snd_simple_pcm_cmd(&ctx, VIRTIO_SND_R_PCM_RELEASE, input_stream_id);
    virtio_snd_test_teardown(&ctx);
}

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("virtio-snd-inject/passthrough-playback", test_passthrough_playback);
    qtest_add_func("virtio-snd-inject/passthrough-capture", test_passthrough_capture);
    return g_test_run();
}
