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

#pragma once

// In-process fakes of the gfxstream host interfaces (RenderChannel, Renderer,
// RenderLib) shared by the goldfish_gpu component tests. They model just
// enough of RenderChannelImpl's event semantics and of the Renderer snapshot
// hooks for the tables and the facade to be exercised without a GPU.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "gfxstream/host/features.h"
#include "render-utils/RenderChannel.h"
#include "render-utils/RenderLib.h"
#include "render-utils/Renderer.h"

namespace goldfish::legacy::testing {

class MockRenderChannel final : public gfxstream::RenderChannel {
  public:
    explicit MockRenderChannel(gfxstream::Stream* load_stream) {
        if (load_stream != nullptr) {
            saved_tag_ = load_stream->getBe32();
        }
    }

    void setEventCallback(EventCallback&& callback) ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        event_cb_ = std::move(callback);
    }

    // Mirrors RenderChannelImpl::setWantedEvents(): OR into the wanted set and
    // deliver (and clear) the bits that are already satisfied.
    void setWantedEvents(State state) ABSL_LOCKS_EXCLUDED(mu_) override {
        EventCallback cb;
        State available = State::Empty;
        {
            absl::MutexLock lock(&mu_);
            wanted_events_ = Or(wanted_events_, state);
            available = TakeAvailableLocked();
            if (available != State::Empty) {
                cb = event_cb_;
            }
        }
        if (cb) {
            cb(available);
        }
    }

    State state() const ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        return StateLocked();
    }

    IoResult tryWrite(Buffer&& buffer) ABSL_LOCKS_EXCLUDED(mu_) override {
        EventCallback cb;
        State available = State::Empty;
        std::function<void()> on_full;
        bool full = false;
        {
            absl::MutexLock lock(&mu_);
            if (stopped_) {
                return IoResult::Error;
            }
            full = to_guest_.size() >= capacity_;
            if (full) {
                on_full = std::move(on_full_);  // one-shot
                on_full_ = nullptr;
            } else {
                // Echo back to guest queue so reader thread can read it.
                to_guest_.push_back(std::move(buffer));
                available = TakeAvailableLocked();
                if (available != State::Empty) {
                    cb = event_cb_;
                }
            }
        }
        if (on_full) {
            // Test hook: runs after a full write was observed and before the
            // caller can re-arm CanWrite (the lost-wakeup window).
            on_full();
        }
        if (cb) {
            cb(available);
        }
        return full ? IoResult::TryAgain : IoResult::Ok;
    }

    void waitUntilWritable() override {}

    IoResult tryRead(Buffer* buffer) ABSL_LOCKS_EXCLUDED(mu_) override {
        EventCallback cb;
        State available = State::Empty;
        {
            absl::MutexLock lock(&mu_);
            if (stopped_) {
                return IoResult::Error;
            }
            if (to_guest_.empty()) {
                return IoResult::TryAgain;
            }
            *buffer = std::move(to_guest_.front());
            to_guest_.pop_front();
            available = TakeAvailableLocked();
            if (available != State::Empty) {
                cb = event_cb_;
            }
        }
        if (cb) {
            cb(available);
        }
        return IoResult::Ok;
    }

    IoResult readBefore(Buffer* buffer, Duration /*waitUntilUs*/) override {
        return tryRead(buffer);
    }

    void waitUntilReadable() override {}

    void stop() ABSL_LOCKS_EXCLUDED(mu_) override {
        EventCallback cb;
        {
            absl::MutexLock lock(&mu_);
            stopped_ = true;
            cb = event_cb_;
        }
        if (cb) {
            cb(State::Stopped);
        }
    }

    void onSave(gfxstream::Stream* stream) override { stream->putBe32(saved_tag_); }

    uint32_t SavedTag() const { return saved_tag_; }

    // Maximum number of queued buffers before tryWrite() reports TryAgain.
    void SetCapacity(size_t capacity) ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        capacity_ = capacity;
    }

    // Runs once, from the next tryWrite() that finds the channel full, after
    // the full state was observed and before TryAgain is returned. Lets a test
    // free space exactly inside the window between the failed write and the
    // caller's CanWrite re-arm.
    void SetOnceOnFullWrite(std::function<void()> hook) ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        on_full_ = std::move(hook);
    }

    // Pops one queued buffer without going through the reader thread.
    bool PopOneForTest() ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        if (to_guest_.empty()) {
            return false;
        }
        to_guest_.pop_front();
        return true;
    }

    size_t QueuedForTest() const ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        return to_guest_.size();
    }

  private:
    static State Or(State a, State b) {
        return static_cast<State>(static_cast<int>(a) | static_cast<int>(b));
    }
    static State And(State a, State b) {
        return static_cast<State>(static_cast<int>(a) & static_cast<int>(b));
    }

    State StateLocked() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
        if (stopped_) {
            return State::Stopped;
        }
        State s = State::Empty;
        if (!to_guest_.empty()) {
            s = Or(s, State::CanRead);
        }
        if (to_guest_.size() < capacity_) {
            s = Or(s, State::CanWrite);
        }
        return s;
    }

    // RenderChannelImpl::notifyStateChangeLocked(): bits that are both wanted
    // and available are reported once and removed from the wanted set.
    State TakeAvailableLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
        const State available = And(StateLocked(), wanted_events_);
        wanted_events_ =
                static_cast<State>(static_cast<int>(wanted_events_) & ~static_cast<int>(available));
        return available;
    }

    mutable absl::Mutex mu_;
    EventCallback event_cb_ ABSL_GUARDED_BY(mu_);
    State wanted_events_ ABSL_GUARDED_BY(mu_) = State::Empty;
    std::deque<Buffer> to_guest_ ABSL_GUARDED_BY(mu_);
    size_t capacity_ ABSL_GUARDED_BY(mu_) = 1024;
    std::function<void()> on_full_ ABSL_GUARDED_BY(mu_);
    bool stopped_ ABSL_GUARDED_BY(mu_) = false;
    uint32_t saved_tag_ = 0xCAFE1234;
};

struct MockAsgConsumer {
    gfxstream::AsgConsumerCreateInfo info;
    uint32_t saved_marker = 0xA5600001;
    bool pre_saved = false;
    bool post_saved = false;
    bool post_load_registered = false;
    bool ring_config_reloaded = false;
};

class MockRenderer final : public gfxstream::Renderer {
  public:
    gfxstream::RenderChannelPtr createRenderChannel(gfxstream::Stream* load_stream,
                                                    uint32_t /*virtioGpuContextId*/) override {
        auto ch = std::make_shared<MockRenderChannel>(load_stream);
        channels.push_back(ch);
        return ch;
    }

    void* addressSpaceGraphicsConsumerCreate(const gfxstream::AsgConsumerCreateInfo& info,
                                             gfxstream::Stream* load_stream) override {
        auto consumer = std::make_unique<MockAsgConsumer>();
        consumer->info = info;
        if (load_stream != nullptr) {
            consumer->saved_marker = load_stream->getBe32();
        }
        MockAsgConsumer* raw = consumer.get();
        asg_consumers.push_back(std::move(consumer));
        return raw;
    }

    void addressSpaceGraphicsConsumerDestroy(void* ptr) override {
        auto* target = static_cast<MockAsgConsumer*>(ptr);
        // Record every destroy (including of an already-destroyed pointer, which
        // the real backend would turn into a double delete) and hold the slot
        // open for a while so that a concurrent second destroyer has a wide
        // window to observe the still-live pointer.
        {
            absl::MutexLock lock(&destroy_mu);
            ++destroy_counts[ptr];
            if (on_destroy_started != nullptr && !on_destroy_started->HasBeenNotified()) {
                on_destroy_started->Notify();
            }
        }
        absl::SleepFor(destroy_delay);
        absl::MutexLock lock(&destroy_mu);
        for (auto it = asg_consumers.begin(); it != asg_consumers.end(); ++it) {
            if (it->get() == target) {
                // Keep the object alive so that its address is never reused by
                // a later consumer and destroy_counts stay per-consumer.
                destroyed_consumers.push_back(std::move(*it));
                asg_consumers.erase(it);
                return;
            }
        }
    }

    int DestroyCount(void* ptr) ABSL_LOCKS_EXCLUDED(destroy_mu) {
        absl::MutexLock lock(&destroy_mu);
        auto it = destroy_counts.find(ptr);
        return it == destroy_counts.end() ? 0 : it->second;
    }

    void addressSpaceGraphicsConsumerPreSave(void* ptr) override {
        static_cast<MockAsgConsumer*>(ptr)->pre_saved = true;
    }

    void addressSpaceGraphicsConsumerSave(void* ptr, gfxstream::Stream* stream) override {
        stream->putBe32(static_cast<MockAsgConsumer*>(ptr)->saved_marker);
    }

    void addressSpaceGraphicsConsumerPostSave(void* ptr) override {
        static_cast<MockAsgConsumer*>(ptr)->post_saved = true;
    }

    void addressSpaceGraphicsConsumerRegisterPostLoadRenderThread(void* ptr) override {
        static_cast<MockAsgConsumer*>(ptr)->post_load_registered = true;
    }

    void addressSpaceGraphicsConsumerReloadRingConfig(void* ptr) override {
        static_cast<MockAsgConsumer*>(ptr)->ring_config_reloaded = true;
    }

    HardwareStrings getHardwareStrings() override { return {"Google", "Mock", "1.0"}; }

    bool getVulkanEmulationDeviceInfo(char**, char**, uint32_t*, uint32_t*, uint32_t*, uint32_t*,
                                      uint32_t*, uint64_t*) override {
        return false;
    }

    void setPostCallback(OnPostCallback post_cb, void* context, bool bgra_readback,
                         uint32_t display_id) override {
        on_post = post_cb;
        on_post_context = context;
        use_bgra_readback = bgra_readback;
        post_display_id = display_id;
    }

    void addListener(FrameBufferChangeEventListener*) override {}
    void removeListener(FrameBufferChangeEventListener*) override {}

    bool asyncReadbackSupported() override { return async_readback_supported; }

    ReadPixelsCallback getReadPixelsCallback() override {
        return [](void* pixels, uint32_t bytes, uint32_t /*displayId*/) {
            auto* out = static_cast<uint8_t*>(pixels);
            for (uint32_t i = 0; i < bytes; ++i) {
                out[i] = static_cast<uint8_t>(i & 0x7f);
            }
        };
    }

    FlushReadPixelPipeline getFlushReadPixelPipeline() override {
        return [](int display_id) { last_flushed_display_id_ = display_id; };
    }

    bool showOpenGLSubwindow(FBNativeWindowType, int, int, int, int, int, int, float, float, bool,
                             bool) override {
        return false;
    }
    bool destroyOpenGLSubwindow() override { return false; }
    void setOpenGLDisplayRotation(float) override {}
    void setOpenGLDisplayTranslation(float, float) override {}
    void repaintOpenGLDisplay() override {}
    bool hasGuestPostedAFrame() override { return false; }
    void resetGuestPostedAFrame() override {}
    void setScreenMask(int, int, const uint8_t*) override {}
    void setScreenBackground(int, int, const uint8_t*) override {}
    void setDisplayLayout(int, int, const gfxstream::Rect&) override {}
    void setMultiDisplay(uint32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t, bool) override {}
    void setMultiDisplayColorBuffer(uint32_t, uint32_t) override {}

    void onGuestGraphicsProcessCreate(uint64_t puid) override { created_puids.push_back(puid); }
    void cleanupProcGLObjects(uint64_t puid) override { cleaned_puids.push_back(puid); }
    bool waitForProcessCleanup() override { return true; }
    struct AndroidVirtioGpuOps* getVirtioGpuOps() override { return nullptr; }

    void stop(bool /*wait*/) override { stopped = true; }
    void finish() override {}
    bool pauseAllPreSave() override {
        paused_all = true;
        return true;
    }
    void resumeAll() override { resumed_all = true; }

    bool save(gfxstream::Stream* stream, const gfxstream::ITextureSaverPtr& tex_saver) override {
        ++save_calls;
        stream->putBe32(0x524E4452);  // "RNDR"
        tex_saver->saveTexture(7, [](gfxstream::Stream* s, gfxstream::ITextureSaver::Buffer*) {
            s->putBe32(0x54455837);  // "TEX7"
        });
        return !fail_save;
    }
    // When set, save() still writes its payload but reports failure, so the
    // caller's cleanup of half-written artifacts can be exercised.
    bool fail_save = false;
    int save_calls = 0;

    bool load(gfxstream::Stream* stream, const gfxstream::ITextureLoaderPtr& tex_loader) override {
        if (stream->getBe32() != 0x524E4452) {
            return false;
        }
        if (!tex_loader->start()) {
            return false;
        }
        captured_texture_loader_wptr = tex_loader;
        uint32_t loaded_tex = 0;
        tex_loader->loadTexture(7,
                                [&loaded_tex](gfxstream::Stream* s) { loaded_tex = s->getBe32(); });
        loaded_texture_magic = loaded_tex;
        return loaded_tex == 0x54455837;
    }

    void fillGLESUsages(android_studio::EmulatorGLESUsages*) override {}
    int getScreenshot(unsigned int, unsigned int*, unsigned int*, uint8_t*, size_t*, int, int, int,
                      int, gfxstream::Rect) override {
        return 0;
    }
    void preLoad() override { pre_loaded = true; }
    void postLoad() override { post_loaded = true; }
    void setVsyncHz(int) override {}
    void setDisplayConfigs(int, int, int, int, int) override {}
    void setDisplayActiveConfig(int) override {}
    const void* getEglDispatch() override { return nullptr; }
    const void* getGles2Dispatch() override { return nullptr; }
    void setShouldSkipDraw(bool) override {}
    bool getShouldSkipDraw() const override { return false; }

    void SimulatePostFrame(int w, int h, unsigned char* pixels) {
        if (on_post != nullptr) {
            on_post(on_post_context, 0, w, h, -1, 0, 0, pixels);
        }
    }

    static inline int last_flushed_display_id_ = -1;
    bool async_readback_supported = false;
    OnPostCallback on_post = nullptr;
    void* on_post_context = nullptr;
    bool use_bgra_readback = false;
    uint32_t post_display_id = 0;
    bool stopped = false;
    bool paused_all = false;
    bool resumed_all = false;
    bool pre_loaded = false;
    bool post_loaded = false;
    uint32_t loaded_texture_magic = 0;
    gfxstream::ITextureLoaderWPtr captured_texture_loader_wptr;
    std::vector<uint64_t> created_puids;
    std::vector<uint64_t> cleaned_puids;
    std::vector<std::unique_ptr<MockAsgConsumer>> asg_consumers;
    std::vector<std::shared_ptr<MockRenderChannel>> channels;
    absl::Mutex destroy_mu;
    std::map<void*, int> destroy_counts ABSL_GUARDED_BY(destroy_mu);
    std::vector<std::unique_ptr<MockAsgConsumer>> destroyed_consumers ABSL_GUARDED_BY(destroy_mu);
    absl::Notification* on_destroy_started ABSL_GUARDED_BY(destroy_mu) = nullptr;
    absl::Duration destroy_delay = absl::ZeroDuration();
};

class MockRenderLib final : public gfxstream::RenderLib {
  public:
    explicit MockRenderLib(std::shared_ptr<MockRenderer> r) : renderer(std::move(r)) {}

    void setRenderer(SelectedRenderer sel) override { selected_renderer = sel; }
    void setGuestAndroidApiLevel(int api) override { api_level = api; }
    void getGlesVersion(int* maj, int* min) override {
        *maj = 3;
        *min = 0;
    }
    void setLogger(gfxstream_log_callback_t) override {}
    void setLogLevel(gfxstream_logging_level) override {}
    void setSyncDevice(gfxstream_sync_create_timeline_t create_timeline,
                       gfxstream_sync_create_fence_t, gfxstream_sync_timeline_inc_t timeline_inc,
                       gfxstream_sync_destroy_timeline_t,
                       gfxstream_sync_register_trigger_wait_t register_trigger_wait,
                       gfxstream_sync_device_exists_t device_exists) override {
        sync_create_timeline = create_timeline;
        sync_timeline_inc = timeline_inc;
        sync_device_exists = device_exists;
        if (register_trigger_wait != nullptr) {
            register_trigger_wait(&MockRenderLib::OnTriggerWait);
        }
    }
    void setDmaOps(gfxstream_dma_ops ops) override { dma_ops = ops; }
    void setVmOps(const gfxstream_vm_ops& ops) override { vm_ops = ops; }
    void setAddressSpaceDeviceControlOps(struct address_space_device_control_ops* ops) override {
        asg_ops = ops;
    }
    void setWindowOps(const gfxstream_window_ops&) override {}
    void setDisplayOps(const gfxstream_multi_display_ops&) override {}
    void setGrallocImplementation(GrallocImplementation g) override { gralloc = g; }
    bool getOpt(gfxstream::RenderOpt*) override { return false; }

    gfxstream::RendererPtr initRenderer(int w, int h, const gfxstream::host::FeatureSet& feat,
                                        bool sub_window) override {
        width = w;
        height = h;
        features = feat;
        use_sub_window = sub_window;
        return renderer;
    }

    gfxstream::OnLastColorBufferRef getOnLastColorBufferRef() override {
        return [this](uint32_t handle) { last_color_buffer_ref = handle; };
    }

    static void OnTriggerWait(uint64_t glsync, uint64_t thread, uint64_t timeline) {
        last_glsync_ = glsync;
        last_thread_ = thread;
        last_timeline_ = timeline;
    }

    static inline uint64_t last_glsync_ = 0;
    static inline uint64_t last_thread_ = 0;
    static inline uint64_t last_timeline_ = 0;

    std::shared_ptr<MockRenderer> renderer;
    SelectedRenderer selected_renderer = SELECTED_RENDERER_UNKNOWN;
    int api_level = 0;
    GrallocImplementation gralloc = MINIGBM;
    int width = 0;
    int height = 0;
    bool use_sub_window = true;
    gfxstream::host::FeatureSet features;
    uint32_t last_color_buffer_ref = 0;
    gfxstream_dma_ops dma_ops = {};
    gfxstream_vm_ops vm_ops = {};
    struct address_space_device_control_ops* asg_ops = nullptr;
    gfxstream_sync_create_timeline_t sync_create_timeline = nullptr;
    gfxstream_sync_timeline_inc_t sync_timeline_inc = nullptr;
    gfxstream_sync_device_exists_t sync_device_exists = nullptr;
};

}  // namespace goldfish::legacy::testing
