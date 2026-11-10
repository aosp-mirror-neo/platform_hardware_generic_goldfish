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

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"

#include "android/base/system.h"
#include "android/status/status_macros.h"
#include "gfxstream/host/features.h"
#include "goldfish/legacy/goldfish_gpu/goldfish_gpu_c_api.h"
#include "goldfish/legacy/goldfish_gpu/renderer/gfxstream_library_loader.h"
#include "goldfish/legacy/goldfish_gpu/renderer/render_lib_bridge.h"
#include "goldfish/legacy/goldfish_gpu/renderer/renderer_config.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/gpu_snapshot_codec.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/gpu_snapshot_files.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/posted_frame_cache.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/snapshot_phase_tracker.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/snapshot_streams.h"
#include "goldfish/legacy/goldfish_gpu/transport/asg_blob_table.h"
#include "goldfish/legacy/goldfish_gpu/transport/opengles_pipe_table.h"
#include "goldfish/legacy/goldfish_gpu/verify/goldfish_gpu_verifier.h"
#include "render-utils/RenderLib.h"
#include "render-utils/Renderer.h"
#include "render-utils/snapshot_operations.h"
#include "render-utils/sync_device.h"

// The process-wide GoldfishGpu: the IGoldfishGpu facade that the legacy
// goldfish_pipe / goldfish_sync / goldfish_address_space devices and the QEMU
// VMState shim (goldfish_gpu_c_api.cc) talk to. It owns the gfxstream
// RenderLib/Renderer and sequences the snapshot steps documented in
// goldfish_gpu_c_api.h; everything else is delegated:
//
//   GfxstreamLibraryLoader  finds and dlopens libgfxstream_backend
//   renderer_config.h       ANDROID_EMU_RENDERER / feature-set / verify-mode parsing
//   RenderLibBridge         the C function-pointer tables gfxstream calls back through
//   AsgBlobTable            Address Space Graphics blobs and their consumers
//   OpenglesPipeTable       "pipe:opengles" render channels and reader threads
//   PostedFrameCache        the last posted frame (saved, replayed after a load)
//   SnapshotPhaseTracker    kRunning / kQuiesced / kSaved / kPreLoaded
//   gpu_snapshot_codec.h    the VMState meta blob format
//   GpuSnapshotFiles        gfxstream_render.bin / gfxstream_textures.bin on disk
//   goldfish_gpu_verifier.h invariant checks over read-only views of the above
//
// Locking: `mu_` guards only the facade's own fields (renderer, config,
// callbacks, phase) and is never held while a component or the renderer is
// called, except for the AsgBlobTable::Close() in CloseAsg(), which must be
// atomic with the phase check and never blocks. Each table has its own mutex;
// none of them calls back into the facade.

namespace goldfish::legacy {
namespace {

// Runs Renderer::cleanupProcGLObjects() for closed GLProcessPipes on its own
// thread. gfxstream's cleanupGuestProcess() spins until every render thread of
// that guest process has exited, and those threads only exit once the guest has
// closed their opengles pipes (an MMIO write that needs the BQL). Doing the
// cleanup on the vCPU thread that closed the GLProcessPipe (BQL held) therefore
// deadlocks whenever a dying process closes the GLProcessPipe before its last
// opengles pipe; the render threads also take the BQL themselves for BAR 1
// (un)mappings. The queue keeps the vCPU thread free and preserves order.
class ProcessCleanupQueue {
  public:
    ~ProcessCleanupQueue() { Join(); }

    void Enqueue(gfxstream::RendererPtr renderer, uint64_t puid) ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        pending_.push_back({std::move(renderer), puid});
        if (!thread_.joinable()) {
            thread_ = std::thread([this] { Run(); });
        }
    }

    // Blocks until every queued cleanup has run. Only call once the render
    // threads can no longer block (after Renderer::stop()), never with the BQL.
    void Drain() ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        mu_.Await(absl::Condition(
                +[](ProcessCleanupQueue* self) ABSL_EXCLUSIVE_LOCKS_REQUIRED(self->mu_) {
                    return self->pending_.empty() && !self->in_flight_;
                },
                this));
    }

    size_t PendingForTesting() ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        return pending_.size() + (in_flight_ ? 1 : 0);
    }

  private:
    struct Item {
        gfxstream::RendererPtr renderer;
        uint64_t puid;
    };

    void Run() ABSL_LOCKS_EXCLUDED(mu_) {
        while (std::optional<Item> item = NextItem()) {
            item->renderer->cleanupProcGLObjects(item->puid);
            item.reset();  // drop the RendererPtr before reporting completion
            absl::MutexLock lock(&mu_);
            in_flight_ = false;
        }
    }

    // Blocks until there is work or Join() was called; nullopt means stop.
    std::optional<Item> NextItem() ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        mu_.Await(absl::Condition(
                +[](ProcessCleanupQueue* self) ABSL_EXCLUSIVE_LOCKS_REQUIRED(self->mu_) {
                    return !self->pending_.empty() || self->stopping_;
                },
                this));
        if (pending_.empty()) {
            return std::nullopt;
        }
        Item item = std::move(pending_.front());
        pending_.pop_front();
        in_flight_ = true;
        return item;
    }

    void Join() ABSL_LOCKS_EXCLUDED(mu_) {
        std::thread thread;
        {
            absl::MutexLock lock(&mu_);
            stopping_ = true;
            thread = std::move(thread_);
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    absl::Mutex mu_;
    std::deque<Item> pending_ ABSL_GUARDED_BY(mu_);
    bool in_flight_ ABSL_GUARDED_BY(mu_) = false;
    bool stopping_ ABSL_GUARDED_BY(mu_) = false;
    std::thread thread_ ABSL_GUARDED_BY(mu_);
};

class GoldfishGpu final : public IGoldfishGpu, public HostCallbacks {
  public:
    static GoldfishGpu& Instance();

    GoldfishGpu() : asgs_([this](uint64_t gpa) { return ResolveDmaHostAddr(gpa); }) {
        RenderLibBridge::Install(this);
    }

    void SetRenderLibFactory(RenderLibFactory factory) ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        render_lib_factory_ = std::move(factory);
    }

    void SetVerifyMode(GoldfishGpuVerifyMode mode) ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        config_.verify_mode = mode;
    }

    GoldfishGpuVerifyMode GetVerifyMode() const ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        return config_.verify_mode;
    }

    absl::Status VerifySteadyState() ABSL_LOCKS_EXCLUDED(mu_) override {
        return VerifyState(VerifyPhase::kSteady);
    }

    absl::Status Start(GoldfishGpuConfig config) ABSL_LOCKS_EXCLUDED(mu_) override {
        if (config.width == 0 || config.height == 0) {
            return absl::InvalidArgumentError(
                    absl::StrCat("GoldfishGpu::Start requires non-zero dimensions (got ",
                                 config.width, "x", config.height, ")"));
        }
        if (config.guest_api_level == 0) {
            return absl::InvalidArgumentError(
                    "GoldfishGpu::Start requires a non-zero guest_api_level (the AVD's Android "
                    "API level)");
        }
        config.verify_mode = ParseVerifyMode(
                android::base::System::Get()->EnvGet("ANDROID_EMU_GOLDFISH_GPU_VERIFY"),
                config.verify_mode);

        Stop();

        gfxstream::host::FeatureSet features;
        const bool egl_on_egl = android::base::System::Get()->EnvGet("ANDROID_EGL_ON_EGL") == "1";
        RETURN_IF_ERROR(BuildFeatureSet(config.renderer_features, egl_on_egl, &features));

        android::base::System::Get()->EnvSet("ANDROID_EMU_HEADLESS", "1");

        RenderLibFactory factory;
        {
            absl::MutexLock lock(&mu_);
            factory = render_lib_factory_;
        }

        gfxstream::RenderLibPtr render_lib;
        if (factory) {
            render_lib = factory();
            if (!render_lib) {
                return absl::InternalError("RenderLibFactory returned nullptr");
            }
        } else {
            android::base::System* system = android::base::System::Get();
            ApplySoftwareEglDefaults(system);
            absl::StatusOr<gfxstream::RenderLibPtr> loaded =
                    library_loader_.Load(system->EnvGet("ANDROID_EMULATOR_LIBRARY_DIR"));
            if (!loaded.ok()) {
                return loaded.status();
            }
            render_lib = *std::move(loaded);
        }

        RenderLibBridge::Configure(
                render_lib.get(), config.guest_api_level,
                ParseRendererBackend(android::base::System::Get()->EnvGet("ANDROID_EMU_RENDERER")));

        gfxstream::RendererPtr renderer = render_lib->initRenderer(
                static_cast<int>(config.width), static_cast<int>(config.height), features,
                /*useSubWindow=*/false);
        if (!renderer) {
            return absl::InternalError(absl::StrCat(
                    "gfxstream RenderLib::initRenderer(", config.width, "x", config.height,
                    ", features='", config.renderer_features, "') failed"));
        }

        {
            absl::MutexLock lock(&mu_);
            config_ = std::move(config);
            render_lib_ = std::move(render_lib);
            renderer_ = renderer;
            on_last_color_buffer_ref_ = render_lib_->getOnLastColorBufferRef();
            post_source_.read_pixels = renderer_->asyncReadbackSupported()
                                               ? renderer_->getReadPixelsCallback()
                                               : nullptr;
            post_source_.before_readback = GlFlushBeforeReadback();
            post_source_.on_post = config_.on_post;
            flush_read_pixels_ = renderer_->getFlushReadPixelPipeline();
            frame_cache_.Clear();
            color_buffers_.clear();
            next_puid_.store(1, std::memory_order_relaxed);
            next_timeline_id_.store(1, std::memory_order_relaxed);
        }

        renderer->setPostCallback(&GoldfishGpu::OnPostCallback, this,
                                  /*useBgraReadback=*/true, /*displayId=*/0);
        return VerifyState(VerifyPhase::kSteady);
    }

    void Stop() ABSL_LOCKS_EXCLUDED(mu_) override { StopImpl(/*destroy_renderer=*/true); }

    void StopForProcessExit() ABSL_LOCKS_EXCLUDED(mu_) override {
        StopImpl(/*destroy_renderer=*/false);
    }

    // `destroy_renderer == false` stops every render thread but intentionally
    // leaks the Renderer and RenderLib: their destructors run
    // FrameBuffer::finalize(), which destroys every EGL context from the
    // calling thread and asserts (EglContext::~EglContext) at QEMU exit, when
    // the display is already being torn down. virtio-gpu-gfxstream never
    // finalizes the renderer at exit either.
    void StopImpl(bool destroy_renderer) ABSL_LOCKS_EXCLUDED(mu_) {
        gfxstream::ITextureLoaderPtr old_texture_loader;
        // Same order as in Start(): the Renderer is destroyed before the
        // RenderLib it came from when they go out of scope below.
        gfxstream::RenderLibPtr render_lib;
        gfxstream::RendererPtr renderer;
        bool quiesced = false;
        const std::vector<std::shared_ptr<AsgConsumerEntry>> asgs = asgs_.Reset();
        {
            absl::MutexLock lock(&mu_);
            old_texture_loader = std::move(active_texture_loader_);
            quiesced = phase_.IsQuiescedForSave();
            phase_.Reset();
            renderer = std::move(renderer_);
            render_lib = std::move(render_lib_);
            on_last_color_buffer_ref_ = nullptr;
            post_source_ = PostedFrameCache::Source{};
            flush_read_pixels_ = nullptr;
            frame_cache_.Clear();
            color_buffers_.clear();
            next_puid_.store(1, std::memory_order_relaxed);
            next_timeline_id_.store(1, std::memory_order_relaxed);
        }

        pipes_.Reset();

        if (old_texture_loader) {
            old_texture_loader->interrupt();
            old_texture_loader->join();
            old_texture_loader.reset();
        }

        if (renderer) {
            renderer->setPostCallback(nullptr, nullptr, /*useBgraReadback=*/false, /*displayId=*/0);
            // A render thread paused by Quiesce() (e.g. the VM was stopped
            // with RUN_STATE_SHUTDOWN and no exit snapshot was saved) parks in
            // gfxstream's doSnapshotOp() until it is resumed, and
            // addressSpaceGraphicsConsumerDestroy()/stop() wait for it to finish.
            UnparkRenderThreadsIfQuiesced(renderer, quiesced);
            AsgBlobTable::DestroyConsumers(renderer.get(), asgs);
            renderer->stop(/*wait=*/true);
            // The render threads are gone, so pending process cleanups can no
            // longer block on them; finish them before the renderer goes away.
            process_cleanup_.Drain();
        }

        if (!destroy_renderer) {
            // Process exit (see the comment above): destroying the Renderer
            // would run FrameBuffer::finalize() and assert, so park both in
            // never-destroyed statics and let the OS reclaim them.
            static absl::NoDestructor<std::vector<gfxstream::RendererPtr>> leaked_renderers;
            static absl::NoDestructor<std::vector<gfxstream::RenderLibPtr>> leaked_libs;
            if (renderer) {
                leaked_renderers->push_back(std::move(renderer));
            }
            if (render_lib) {
                leaked_libs->push_back(std::move(render_lib));
            }
        }
    }

    void FlushReadPixelPipeline() ABSL_LOCKS_EXCLUDED(mu_) override {
        gfxstream::Renderer::FlushReadPixelPipeline flush_fn = nullptr;
        {
            absl::MutexLock lock(&mu_);
            flush_fn = flush_read_pixels_;
        }
        if (flush_fn != nullptr) {
            flush_fn(/*displayId=*/0);
        }
    }

    // Pauses the renderer, the ASG consumers and the opengles reader threads.
    // Idempotent: a second call while already paused (e.g. the VM change-state
    // handler's Quiesce followed by the "goldfish_gpu_begin" section's
    // pre_save) is a no-op. `snapshot_name` is only forwarded to the PostSave()
    // that undoes a failed pause, which ignores it.
    absl::Status Quiesce(std::string_view snapshot_name) ABSL_LOCKS_EXCLUDED(mu_) override {
        bool already_quiesced = false;
        {
            absl::MutexLock lock(&mu_);
            if (!renderer_) {
                return absl::OkStatus();
            }
            already_quiesced = phase_.IsQuiescedForSave();
        }
        if (already_quiesced) {
            return VerifyState(VerifyPhase::kPostQuiesce, snapshot_name);
        }
        RETURN_IF_ERROR(VerifyState(VerifyPhase::kSteady, snapshot_name));

        gfxstream::RendererPtr renderer;
        gfxstream::ITextureLoaderPtr active_texture_loader;
        {
            absl::MutexLock lock(&mu_);
            if (!renderer_ || !phase_.Enter(SnapshotPhase::kQuiesced, {SnapshotPhase::kRunning})) {
                return absl::OkStatus();
            }
            renderer = renderer_;
            active_texture_loader = active_texture_loader_;
        }

        if (active_texture_loader) {
            active_texture_loader->join();
        }

        pipes_.PauseAll();
        asgs_.PauseAll(*renderer);
        if (!renderer->pauseAllPreSave()) {
            PostSave(snapshot_name).IgnoreError();
            return absl::InternalError("gfxstream Renderer::pauseAllPreSave failed");
        }
        // pauseAllPreSave() and addressSpaceGraphicsConsumerPreSave() only
        // signal the render threads to pause; they finish the packet they are
        // decoding first and park inside RenderThread::doSnapshotOp()
        // (SnapshotState::Finished). Wait for every ASG consumer and render
        // channel to reach that parked state and flush active ColorBuffers
        // before returning, so no GPU thread is still touching guest RAM or
        // BAR 1 when QEMU starts its RAM save pass.
        std::vector<uint8_t> wait_sink_bytes;
        BufferStream wait_sink(&wait_sink_bytes);
        asgs_.WaitAllPaused(*renderer, &wait_sink);
        pipes_.WaitAllPaused(*renderer, &wait_sink);
        if (absl::Status st = VerifyState(VerifyPhase::kPostQuiesce, snapshot_name); !st.ok()) {
            PostSave(snapshot_name).IgnoreError();
            return st;
        }
        return absl::OkStatus();
    }

    // Ensures the GPU is quiesced, writes the gfxstream render/texture streams,
    // and encodes the ASG consumers, pipes, frame and counters into the cached
    // meta blob before goldfish_address_space saves the ASG ring storage.
    absl::Status PreSave(std::string_view snapshot_name) ABSL_LOCKS_EXCLUDED(mu_) override {
        RETURN_IF_ERROR(Quiesce(snapshot_name));
        return SaveStreamsAndCacheMeta(snapshot_name);
    }

    // Returns the meta blob cached by PreSave(). Save() is a pure read: it
    // never builds the blob itself, because the gfxstream ASG consumer save
    // must run in the "goldfish_gpu_begin" section's pre_save, before
    // goldfish_address_space snapshots the ASG ring memory. Calling it in any
    // phase other than kSaved is a sequencing bug and fails the section.
    absl::StatusOr<std::vector<uint8_t>> Save(std::string_view snapshot_name)
            ABSL_LOCKS_EXCLUDED(mu_) override {
        {
            absl::MutexLock lock(&mu_);
            if (!renderer_) {
                return std::vector<uint8_t>{};
            }
            if (phase_.phase() != SnapshotPhase::kSaved) {
                return absl::FailedPreconditionError(absl::StrCat(
                        "GoldfishGpu::Save called in snapshot phase ",
                        SnapshotPhaseName(phase_.phase()), "; PreSave must run first"));
            }
        }
        RETURN_IF_ERROR(VerifyState(VerifyPhase::kPostSaveStream, snapshot_name));
        absl::MutexLock lock(&mu_);
        return phase_.CachedMeta();
    }

    // PreSave step 2: writes the gfxstream render/texture streams and encodes
    // the meta blob into the phase tracker's cache (kQuiesced -> kSaved).
    // Idempotent: a repeat call in kSaved only re-verifies.
    absl::Status SaveStreamsAndCacheMeta(std::string_view snapshot_name) ABSL_LOCKS_EXCLUDED(mu_) {
        bool already_saved = false;
        {
            absl::MutexLock lock(&mu_);
            if (!renderer_) {
                return absl::OkStatus();
            }
            already_saved = phase_.phase() == SnapshotPhase::kSaved;
        }
        if (already_saved) {
            return VerifyState(VerifyPhase::kPostSaveStream, snapshot_name);
        }
        RETURN_IF_ERROR(VerifyState(VerifyPhase::kPreSaveStream, snapshot_name));

        gfxstream::RendererPtr renderer;
        std::filesystem::path snapshot_dir;
        const uint32_t saved_next_asg_handle = asgs_.NextId();
        const uint32_t saved_next_pipe_id = pipes_.NextId();
        const uint64_t saved_next_puid = next_puid_.load(std::memory_order_relaxed);
        const uint64_t saved_next_timeline_id = next_timeline_id_.load(std::memory_order_relaxed);
        {
            absl::MutexLock lock(&mu_);
            if (!renderer_ || phase_.phase() == SnapshotPhase::kSaved) {
                return absl::OkStatus();
            }
            renderer = renderer_;
            snapshot_dir = config_.snapshot_directory;
        }

        if (absl::Status st = GpuSnapshotFiles(snapshot_dir).Save(snapshot_name, renderer.get());
            !st.ok()) {
            PostSave(snapshot_name).IgnoreError();
            return st;
        }

        const std::vector<uint32_t> asg_handles = asgs_.SortedIds();
        const std::vector<uint32_t> pipe_ids = pipes_.SortedIds();
        const GpuSnapshotFrame frame = frame_cache_.Last();
        const GpuSnapshotCounters counters{.next_asg_handle = saved_next_asg_handle,
                                           .next_pipe_id = saved_next_pipe_id,
                                           .next_puid = saved_next_puid,
                                           .next_timeline_id = saved_next_timeline_id};
        absl::StatusOr<std::vector<uint8_t>> encoded = EncodeGpuSnapshot(
                asg_handles,
                [&](uint32_t handle, gfxstream::Stream* out) {
                    asgs_.SaveEntry(renderer.get(), handle, out);
                },
                pipe_ids,
                [&](uint32_t id, gfxstream::Stream* out) {
                    pipes_.SaveEntry(renderer.get(), id, out);
                },
                frame, counters);
        if (!encoded.ok()) {
            PostSave(snapshot_name).IgnoreError();
            return encoded.status();
        }
        std::vector<uint8_t> meta_bytes = *std::move(encoded);
        {
            absl::MutexLock lock(&mu_);
            phase_.Enter(SnapshotPhase::kSaved, {SnapshotPhase::kQuiesced});
            phase_.SetCachedMeta(meta_bytes);
        }
        if (absl::Status st = VerifyState(VerifyPhase::kPostSaveStream, snapshot_name); !st.ok()) {
            PostSave(snapshot_name).IgnoreError();
            return st;
        }
        return absl::OkStatus();
    }

    absl::Status PostSave(std::string_view snapshot_name) ABSL_LOCKS_EXCLUDED(mu_) override {
        ResumeAfterSnapshot();
        return VerifyState(VerifyPhase::kPostSave, snapshot_name);
    }

    void OnSnapshotOperationFinished() ABSL_LOCKS_EXCLUDED(mu_) override {
        gfxstream::RendererPtr renderer;
        {
            absl::MutexLock lock(&mu_);
            if (phase_.IsRunning()) {
                return;
            }
            LOG(WARNING) << "goldfish_gpu: snapshot operation finished without "
                         << (phase_.IsPreLoaded() ? "PostLoad" : "PostSave")
                         << " (an earlier VMState section failed); resuming the renderer";
            // Check and reset under one lock hold: this is reachable from both
            // the VM change-state handler and the snapshot-progress bottom
            // half, and exactly one of them must perform the resume.
            phase_.Reset();
            renderer = renderer_;
        }
        ResumeRendererAndTables(renderer);
    }

    // Shared by PostSave() and OnSnapshotOperationFinished(): clears the
    // snapshot-in-progress flags, resumes the renderer and the ASG consumers,
    // and un-pauses the pipe channels (restarting their reader threads when a
    // Load() created them without one).
    void ResumeAfterSnapshot() ABSL_LOCKS_EXCLUDED(mu_) {
        gfxstream::RendererPtr renderer;
        {
            absl::MutexLock lock(&mu_);
            phase_.Reset();
            DropPendingColorBuffersLocked();
            renderer = renderer_;
        }
        ResumeRendererAndTables(renderer);
    }

    // The resume proper, after the phase tracker was reset under mu_.
    void ResumeRendererAndTables(const gfxstream::RendererPtr& renderer) ABSL_LOCKS_EXCLUDED(mu_) {
        if (!renderer) {
            // Nothing to resume; drop the entries retired while quiesced.
            asgs_.DrainRetired(nullptr);
            return;
        }

        renderer->resumeAll();
        asgs_.ResumeAll(*renderer);
        // Blobs retired by CloseAsg() while the GPU was quiesced: their
        // render threads are unparked now, so they can be torn down.
        asgs_.DrainRetired(renderer.get());
        pipes_.ResumeAll();
    }

    absl::Status PreLoad(std::string_view snapshot_name) ABSL_LOCKS_EXCLUDED(mu_) override {
        bool already_pre_loaded = false;
        {
            absl::MutexLock lock(&mu_);
            if (!renderer_) {
                return absl::OkStatus();
            }
            already_pre_loaded = phase_.IsPreLoaded();
        }
        if (already_pre_loaded) {
            return VerifyState(VerifyPhase::kPreLoad, snapshot_name);
        }

        gfxstream::ITextureLoaderPtr old_texture_loader;
        gfxstream::RendererPtr renderer;
        bool quiesced = false;
        {
            absl::MutexLock lock(&mu_);
            if (!renderer_) {
                return absl::OkStatus();
            }
            quiesced = phase_.IsQuiescedForSave();
            if (!phase_.Enter(SnapshotPhase::kPreLoaded,
                              {SnapshotPhase::kRunning, SnapshotPhase::kQuiesced,
                               SnapshotPhase::kSaved})) {
                return absl::OkStatus();
            }
            color_buffers_.clear();
            old_texture_loader = std::move(active_texture_loader_);
            renderer = renderer_;
        }

        if (old_texture_loader) {
            old_texture_loader->interrupt();
            old_texture_loader->join();
            old_texture_loader.reset();
        }

        pipes_.StopAll();
        asgs_.MarkAllExiting();
        // The VM change-state handler quiesces the GPU (Quiesce) when the VM
        // stops for RUN_STATE_RESTORE_VM, before this runs from the
        // "goldfish_gpu_begin" section's pre_load. Paused render threads park
        // in gfxstream's doSnapshotOp() and only leave it through resume(), while
        // addressSpaceGraphicsConsumerDestroy() and preLoad() (cleanupRenderThreads)
        // wait for them to finish: unpark them first. Pausing drained the ASG
        // rings and render channels, and `exiting` is already set, so the threads
        // exit without decoding anything.
        UnparkRenderThreadsIfQuiesced(renderer, quiesced);

        // Includes the blobs qemu_system_reset() retired through the RCU-deferred
        // CloseAsg() while the GPU was quiesced.
        asgs_.StopAll(renderer.get());

        renderer->preLoad();
        if (absl::Status st = VerifyState(VerifyPhase::kPreLoad, snapshot_name); !st.ok()) {
            ResumeAfterSnapshot();
            return st;
        }
        return absl::OkStatus();
    }

    // Resumes every render thread that Quiesce() paused (render channels and ASG
    // consumers) so that it can be torn down. No-op unless `quiesced`.
    static void UnparkRenderThreadsIfQuiesced(const gfxstream::RendererPtr& renderer,
                                              bool quiesced) {
        if (quiesced && renderer) {
            renderer->resumeAll();
        }
    }

    absl::Status Load(std::string_view snapshot_name, const uint8_t* meta_data, size_t meta_size)
            ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::Status st = LoadImpl(snapshot_name, meta_data, meta_size);
        if (!st.ok()) {
            // PreLoad() paused the renderer, the ASG consumers and the pipe
            // channels; PostLoad() will not run after a failed section, and
            // OnSnapshotOperationFinished() only acts while the phase is kPreLoaded
            // still set, so resume (and clear the flags) here.
            ResumeAfterSnapshot();
        }
        return st;
    }

    absl::Status LoadImpl(std::string_view snapshot_name, const uint8_t* meta_data,
                          size_t meta_size) ABSL_LOCKS_EXCLUDED(mu_) {
        gfxstream::RendererPtr renderer;
        std::filesystem::path snapshot_dir;
        {
            absl::MutexLock lock(&mu_);
            if (!renderer_) {
                return absl::OkStatus();
            }
            renderer = renderer_;
            snapshot_dir = config_.snapshot_directory;
        }
        RETURN_IF_ERROR(VerifyState(VerifyPhase::kPreLoad, snapshot_name));

        const GpuSnapshotFiles files(snapshot_dir);
        const bool has_meta = meta_data != nullptr && meta_size != 0;
        if (has_meta && !files.IsOnDisk(snapshot_name)) {
            // Save() in in-memory mode only drives Renderer::save() for its
            // bookkeeping and discards the stream, so there is nothing to
            // Renderer::load() here; recreating ASG consumers and render
            // channels against an unrestored renderer would reference GL
            // state that does not exist. Refuse instead of guessing.
            return absl::FailedPreconditionError(
                    absl::StrCat("GoldfishGpu snapshot '", snapshot_name, "' carries ", meta_size,
                                 " bytes of GPU state but no renderer state is on disk (",
                                 snapshot_dir.empty() ? "no snapshot directory configured"
                                                      : "snapshot has no name",
                                 "); the renderer cannot be restored"));
        }
        absl::StatusOr<gfxstream::ITextureLoaderPtr> texture_loader =
                files.Load(snapshot_name, renderer.get());
        if (!texture_loader.ok()) {
            return texture_loader.status();
        }
        if (*texture_loader) {
            absl::MutexLock lock(&mu_);
            active_texture_loader_ = *std::move(texture_loader);
        }

        if (!has_meta) {
            RETURN_IF_ERROR(pipes_.VerifyNoPendingRestores());
            {
                absl::MutexLock lock(&mu_);
                ActivatePendingColorBuffersLocked();
            }
            return VerifyState(VerifyPhase::kPostLoadStream, snapshot_name);
        }
        absl::StatusOr<GpuSnapshotTail> tail = DecodeGpuSnapshot(
                meta_data, meta_size,
                [&](uint32_t handle, gfxstream::Stream* in) {
                    return asgs_.LoadEntry(renderer.get(), handle, in);
                },
                [&](uint32_t id, gfxstream::Stream* in) {
                    return pipes_.LoadEntry(renderer.get(), id, in);
                });
        if (!tail.ok()) {
            return tail.status();
        }
        RETURN_IF_ERROR(pipes_.VerifyNoPendingRestores());
        {
            absl::MutexLock lock(&mu_);
            ActivatePendingColorBuffersLocked();
        }
        asgs_.RaiseNextId(tail->counters.next_asg_handle);
        pipes_.RaiseNextId(tail->counters.next_pipe_id);
        frame_cache_.Set(std::move(tail->frame));
        RaiseAtomicTo(&next_puid_, tail->counters.next_puid);
        RaiseAtomicTo(&next_timeline_id_, tail->counters.next_timeline_id);
        return VerifyState(VerifyPhase::kPostLoadStream, snapshot_name);
    }

    static void RaiseAtomicTo(std::atomic<uint64_t>* counter, uint64_t value) {
        uint64_t cur = counter->load(std::memory_order_relaxed);
        while (cur < value &&
               !counter->compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
        }
    }

    absl::Status PostLoad(std::string_view snapshot_name) ABSL_LOCKS_EXCLUDED(mu_) override {
        gfxstream::RendererPtr renderer;
        GoldfishPostCallback on_post;
        {
            absl::MutexLock lock(&mu_);
            phase_.Enter(SnapshotPhase::kRunning, {SnapshotPhase::kPreLoaded});
            renderer = renderer_;
            on_post = config_.on_post;
        }
        if (!renderer) {
            asgs_.DrainRetired(nullptr);
            return absl::OkStatus();
        }

        asgs_.ReloadRingConfigs(*renderer);
        renderer->postLoad();
        renderer->resumeAll();
        // Blobs retired by CloseAsg() between Load() and here (none are
        // expected, but a guest-close racing the restore is harmless now).
        asgs_.DrainRetired(renderer.get());
        pipes_.ResumeAll();

        frame_cache_.Replay(on_post);
        return VerifyState(VerifyPhase::kPostLoad, snapshot_name);
    }

    void SetMemoryMappingCallbacks(MapUserMemoryCallback map_cb, UnmapUserMemoryCallback unmap_cb,
                                   ResolveDmaHostAddrCallback resolve_dma_cb)
            ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        map_user_memory_cb_ = std::move(map_cb);
        unmap_user_memory_cb_ = std::move(unmap_cb);
        resolve_dma_cb_ = std::move(resolve_dma_cb);
    }

    absl::StatusOr<AsgBlob> CreateAsgBlob(uint64_t size) override { return asgs_.Create(size); }

    absl::StatusOr<AsgBlob> RestoreAsgBlob(uint32_t context_handle, uint64_t size) override {
        return asgs_.Restore(context_handle, size);
    }

    absl::Status StartAsg(const AsgBlob& blob) ABSL_LOCKS_EXCLUDED(mu_) override {
        gfxstream::RendererPtr renderer = GetRenderer();
        return asgs_.Start(renderer.get(), blob);
    }

    absl::Status PingAsg(const AsgBlob& blob) override { return asgs_.Ping(blob); }

    // Runs with the BQL held (from goldfish_address_space's RCU-deferred
    // teardown), so it must never wait for a render thread that may itself
    // need the BQL. While the GPU is quiesced (Quiesce or PreLoad in progress)
    // the consumer's render thread is parked, so the entry is only unlinked
    // here and its consumer destroyed by whichever of PreLoad(),
    // ResumeAfterSnapshot(), PostLoad() or StopImpl() ends the quiesce.
    void CloseAsg(const AsgBlob& blob) ABSL_LOCKS_EXCLUDED(mu_) override {
        std::shared_ptr<AsgConsumerEntry> entry;
        gfxstream::RendererPtr renderer;
        {
            absl::MutexLock lock(&mu_);
            renderer = renderer_;
            const bool retire = renderer != nullptr && !phase_.IsRunning();
            entry = asgs_.Close(blob, retire);
        }
        if (entry) {
            AsgBlobTable::DestroyConsumers(renderer.get(), {entry});
        }
    }

    void SetTimelineIncCallback(TimelineIncCallback callback) ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        timeline_inc_cb_ = std::move(callback);
    }

    void TriggerHostWait(uint64_t glsync_handle, uint64_t thread_handle,
                         uint64_t guest_timeline_handle) ABSL_LOCKS_EXCLUDED(mu_) override {
        gfxstream_sync_trigger_wait_t trigger_fn = nullptr;
        {
            absl::MutexLock lock(&mu_);
            trigger_fn = trigger_wait_fn_;
        }
        if (trigger_fn != nullptr) {
            trigger_fn(glsync_handle, thread_handle, guest_timeline_handle);
        }
    }

    uint64_t CreateProcessContext() ABSL_LOCKS_EXCLUDED(mu_) override {
        const uint64_t puid = next_puid_.fetch_add(1, std::memory_order_relaxed);
        gfxstream::RendererPtr renderer = GetRenderer();
        if (renderer) {
            renderer->onGuestGraphicsProcessCreate(puid);
        }
        return puid;
    }

    // Called from the vCPU thread (BQL held) when the guest closes a
    // GLProcessPipe; the cleanup itself waits for the process' render threads,
    // so it runs on ProcessCleanupQueue's thread (see there). A cleanup still
    // pending when a snapshot is taken is lost by the load: the pipe is gone,
    // so the process' GL objects stay until the renderer is stopped.
    void DestroyProcessContext(uint64_t puid) ABSL_LOCKS_EXCLUDED(mu_) override {
        gfxstream::RendererPtr renderer = GetRenderer();
        if (renderer) {
            process_cleanup_.Enqueue(std::move(renderer), puid);
        }
    }

    void WaitForProcessCleanupsForTesting() { process_cleanup_.Drain(); }

    // "pipe:refcount": the guest writes the handle of a ColorBuffer whose
    // lifetime is tied to the pipe. Handle 0 is gfxstream's "no ColorBuffer"
    // (what createColorBuffer returns on failure) and is not tracked, so that
    // ColorBufferLastRef() never tells gfxstream to release a buffer that does
    // not exist.
    void ColorBufferRef(uint32_t handle) ABSL_LOCKS_EXCLUDED(mu_) override {
        if (handle == 0) {
            return;
        }
        absl::MutexLock lock(&mu_);
        // While a snapshot is being loaded the refcount pipes are restored
        // before Load() recreates the renderer state; they become active in
        // ActivatePendingColorBuffersLocked() once that succeeded.
        color_buffers_[handle] = !phase_.IsPreLoaded();
    }

    void ColorBufferLastRef(uint32_t handle) ABSL_LOCKS_EXCLUDED(mu_) override {
        if (handle == 0) {
            return;
        }
        gfxstream::OnLastColorBufferRef cb;
        {
            absl::MutexLock lock(&mu_);
            color_buffers_.erase(handle);
            if (!phase_.IsPreLoaded()) {
                cb = on_last_color_buffer_ref_;
            }
        }
        if (cb) {
            cb(handle);
        }
    }

    absl::StatusOr<uint32_t> CreateOpenglesPipe(OpenglesReadCallback on_host_data,
                                                OpenglesWritableCallback on_writable)
            ABSL_LOCKS_EXCLUDED(mu_) override {
        gfxstream::RendererPtr renderer = GetRenderer();
        if (!renderer) {
            return absl::FailedPreconditionError(
                    "GoldfishGpu::CreateOpenglesPipe called before Start()");
        }
        return pipes_.Create(renderer.get(), std::move(on_host_data), std::move(on_writable));
    }

    // Load, device half: goldfish_pipe re-announces a saved pipe id with its
    // callbacks (at MIG_PRI_DEFAULT, before Load() recreates the channel at
    // MIG_PRI_LOW), so the table stages them until LoadEntry(); Load() checks
    // that every staged restore was matched.
    absl::Status RestoreOpenglesPipe(uint32_t id, OpenglesReadCallback on_host_data,
                                     OpenglesWritableCallback on_writable) override {
        return pipes_.Restore(id, std::move(on_host_data), std::move(on_writable));
    }

    // Never blocks; see OpenglesPipeTable.
    absl::Status WriteOpenglesPipe(uint32_t id, const uint8_t* data, size_t size) override {
        return pipes_.Write(id, data, size);
    }

    void CloseOpenglesPipe(uint32_t id) override { pipes_.Close(id); }

  private:
    // HostCallbacks (reached from gfxstream render threads through RenderLibBridge).
    // Returns false (and logs) when no mapping callback is installed or the
    // BAR 1 mapping fails, so gfxstream can report it instead of silently
    // leaving the guest with an unmapped host-visible allocation.
    bool MapUserMemory(uint64_t gpa, void* hva, uint64_t size) ABSL_LOCKS_EXCLUDED(mu_) override {
        MapUserMemoryCallback cb;
        {
            absl::MutexLock lock(&mu_);
            cb = map_user_memory_cb_;
        }
        if (!cb) {
            LOG(ERROR) << "goldfish_gpu: map_user_memory(gpa=0x" << std::hex << gpa << ", size=0x"
                       << size << ", hva=" << hva
                       << ") called with no memory-mapping callback installed";
            return false;
        }
        if (!cb(gpa, hva, size)) {
            LOG(ERROR) << "goldfish_gpu: map_user_memory(gpa=0x" << std::hex << gpa << ", size=0x"
                       << size << ", hva=" << hva << ") failed";
            return false;
        }
        return true;
    }

    bool UnmapUserMemory(uint64_t gpa, uint64_t size) ABSL_LOCKS_EXCLUDED(mu_) override {
        UnmapUserMemoryCallback cb;
        {
            absl::MutexLock lock(&mu_);
            cb = unmap_user_memory_cb_;
        }
        if (!cb) {
            LOG(ERROR) << "goldfish_gpu: unmap_user_memory(gpa=0x" << std::hex << gpa << ", size=0x"
                       << size << ") called with no memory-mapping callback installed";
            return false;
        }
        if (!cb(gpa, size)) {
            LOG(ERROR) << "goldfish_gpu: unmap_user_memory(gpa=0x" << std::hex << gpa << ", size=0x"
                       << size << ") failed";
            return false;
        }
        return true;
    }

    uint64_t CreateSyncTimeline() override {
        return next_timeline_id_.fetch_add(1, std::memory_order_relaxed);
    }

    void SyncTimelineInc(uint64_t timeline, uint32_t how_much) ABSL_LOCKS_EXCLUDED(mu_) override {
        TimelineIncCallback cb;
        {
            absl::MutexLock lock(&mu_);
            cb = timeline_inc_cb_;
        }
        if (cb) {
            cb(timeline, how_much);
        }
    }

    void RegisterSyncTriggerWait(gfxstream_sync_trigger_wait_t trigger_fn)
            ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        trigger_wait_fn_ = trigger_fn;
    }

    bool SyncDeviceExists() ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        return static_cast<bool>(timeline_inc_cb_);
    }

    // Windows/ANGLE: glFlush() on the posting context before the PBO readback,
    // otherwise the readback thread maps a PBO whose glReadPixels has not been
    // submitted yet and the first repaint after a snapshot load shows a stale
    // frame (b/445476051). Resolved from libGLESv2.dll, which gfxstream has
    // already loaded; null (no-op) elsewhere or when the symbol is missing.
    static std::function<void()> GlFlushBeforeReadback() {
#if defined(_WIN32)
        if (HMODULE gles2 = ::GetModuleHandleW(L"libGLESv2.dll"); gles2 != nullptr) {
            using GlFlushFn = void (*)();
            if (auto fn = reinterpret_cast<GlFlushFn>(::GetProcAddress(gles2, "glFlush"))) {
                return fn;
            }
        }
#endif
        return nullptr;
    }

    static void OnPostCallback(void* context, uint32_t display_id, int width, int height,
                               int /*ydir*/, int /*format*/, int /*type*/, unsigned char* pixels) {
        if (context == nullptr) {
            return;
        }
        auto* self = static_cast<GoldfishGpu*>(context);
        PostedFrameCache::Source source;
        {
            absl::MutexLock lock(&self->mu_);
            source = self->post_source_;
        }
        self->frame_cache_.OnPost(source, display_id, width, height, pixels);
    }

    void* ResolveDmaHostAddr(uint64_t gpa) ABSL_LOCKS_EXCLUDED(mu_) override {
        ResolveDmaHostAddrCallback cb;
        {
            absl::MutexLock lock(&mu_);
            cb = resolve_dma_cb_;
        }
        return cb ? cb(gpa) : nullptr;
    }

    // Verifies state invariants across the component tables and the snapshot
    // artifacts (mirroring virtio_gpu_gfxstream_verify); see goldfish_gpu_verifier.h.
    absl::Status VerifyState(VerifyPhase phase, std::string_view snapshot_name = "")
            ABSL_LOCKS_EXCLUDED(mu_) {
        GpuVerifyInput in;
        in.phase = phase;
        std::filesystem::path snapshot_dir;
        {
            absl::MutexLock lock(&mu_);
            if (!renderer_) {
                return absl::OkStatus();
            }
            in.mode = config_.verify_mode;
            if (in.mode == GoldfishGpuVerifyMode::kOff) {
                return absl::OkStatus();
            }
            snapshot_dir = config_.snapshot_directory;
            in.has_renderer = renderer_ != nullptr;
            in.has_render_lib = render_lib_ != nullptr;
            in.width = config_.width;
            in.height = config_.height;
            in.guest_api_level = config_.guest_api_level;
            in.snapshot_phase = phase_.phase();
            in.cached_meta_size = phase_.CachedMeta().size();
            in.has_texture_loader = active_texture_loader_ != nullptr;
            in.active_color_buffer_count = 0;
            for (const auto& [handle, is_active] : color_buffers_) {
                in.active_color_buffer_count += is_active ? 1 : 0;
            }
            in.has_zero_color_buffer_handle = color_buffers_.count(0) != 0;
        }
        in.next_puid = next_puid_.load(std::memory_order_relaxed);
        in.next_timeline_id = next_timeline_id_.load(std::memory_order_relaxed);
        in.asgs = asgs_.Snapshot();
        in.pipes = pipes_.Snapshot();
        in.frame = frame_cache_.LastShape();
        in.snapshot_artifacts = GpuSnapshotFiles(snapshot_dir).ArtifactsPresent(snapshot_name);
        return VerifyGoldfishGpuState(in);
    }

    gfxstream::RendererPtr GetRenderer() const ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        return renderer_;
    }

    // Load succeeded: the ColorBuffers re-announced by the restored refcount
    // pipes are now backed by renderer state.
    void ActivatePendingColorBuffersLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
        for (auto& [handle, is_active] : color_buffers_) {
            is_active = true;
        }
    }

    // Load failed or was abandoned: forget the handles that were only pending.
    void DropPendingColorBuffersLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
        for (auto it = color_buffers_.begin(); it != color_buffers_.end();) {
            it = it->second ? std::next(it) : color_buffers_.erase(it);
        }
    }

    mutable absl::Mutex mu_;
    // Renderer lifetime.
    RenderLibFactory render_lib_factory_ ABSL_GUARDED_BY(mu_);
    GfxstreamLibraryLoader library_loader_;
    GoldfishGpuConfig config_ ABSL_GUARDED_BY(mu_);
    gfxstream::RenderLibPtr render_lib_ ABSL_GUARDED_BY(mu_);
    gfxstream::RendererPtr renderer_ ABSL_GUARDED_BY(mu_);
    gfxstream::OnLastColorBufferRef on_last_color_buffer_ref_ ABSL_GUARDED_BY(mu_);
    gfxstream::Renderer::FlushReadPixelPipeline flush_read_pixels_ ABSL_GUARDED_BY(mu_) = nullptr;
    PostedFrameCache::Source post_source_ ABSL_GUARDED_BY(mu_);

    // Callbacks installed by the goldfish_address_space / goldfish_sync devices.
    MapUserMemoryCallback map_user_memory_cb_ ABSL_GUARDED_BY(mu_);
    UnmapUserMemoryCallback unmap_user_memory_cb_ ABSL_GUARDED_BY(mu_);
    ResolveDmaHostAddrCallback resolve_dma_cb_ ABSL_GUARDED_BY(mu_);
    TimelineIncCallback timeline_inc_cb_ ABSL_GUARDED_BY(mu_);
    gfxstream_sync_trigger_wait_t trigger_wait_fn_ ABSL_GUARDED_BY(mu_) = nullptr;

    // Snapshot state.
    SnapshotPhaseTracker phase_ ABSL_GUARDED_BY(mu_);
    gfxstream::ITextureLoaderPtr active_texture_loader_ ABSL_GUARDED_BY(mu_);

    // goldfish_pipe GPU service bookkeeping: ColorBuffer handle -> is_active
    // (false while only re-announced by a restored refcount pipe during a load).
    std::unordered_map<uint32_t, bool> color_buffers_ ABSL_GUARDED_BY(mu_);
    std::atomic<uint64_t> next_puid_{1};
    std::atomic<uint64_t> next_timeline_id_{1};

    // Components with their own locks (see the file comment).
    AsgBlobTable asgs_;
    OpenglesPipeTable pipes_;
    PostedFrameCache frame_cache_;
    ProcessCleanupQueue process_cleanup_;
};

GoldfishGpu& GoldfishGpu::Instance() {
    static absl::NoDestructor<GoldfishGpu> instance;
    return *instance;
}

}  // namespace

IGoldfishGpu& GetGoldfishGpu() {
    return GoldfishGpu::Instance();
}

void SetRenderLibFactoryForTesting(RenderLibFactory factory) {
    GoldfishGpu::Instance().SetRenderLibFactory(std::move(factory));
}

void WaitForProcessCleanupsForTesting() {
    GoldfishGpu::Instance().WaitForProcessCleanupsForTesting();
}

}  // namespace goldfish::legacy
