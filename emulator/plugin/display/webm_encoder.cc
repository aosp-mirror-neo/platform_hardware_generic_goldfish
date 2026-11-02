// Copyright 2024 The Android Open Source Project
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

#include "goldfish/display/webm_encoder.h"

#include <utility>

#include "absl/log/log.h"

#include "goldfish/perfetto/perfetto_categories.h"

namespace goldfish::display {
namespace {
constexpr size_t kRgbChannels = 3;
}  // namespace

WebMEncoder::WebMEncoder(std::string fname, int w, int h, int f, int br)
        : filename_(std::move(fname)), width_(w), height_(h), fps_(f), bitrate_(br) {
    // Constructor is now safe and lightweight
    VLOG(2) << "[Encoder] VP9 Codec  w " << w << " h " << h << " f " << f << " br " << br;
}

WebMEncoder::~WebMEncoder() {
    if (is_initialized_ && !finished_) {
        Finish();
    }
    // Join thread if it's still running (just in case Finish wasn't called explicitly)
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    Cleanup();
}

bool WebMEncoder::FailInit(std::string_view reason, int av_err) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_err < 0) {
        av_strerror(av_err, err_buf, sizeof(err_buf));
    }
    LOG(WARNING) << "[Encoder] " << reason << " for '" << filename_ << "' (" << width_ << "x"
                 << height_ << ")" << (av_err < 0 ? ": " : "") << err_buf;
    Cleanup();
    return false;
}

bool WebMEncoder::Init() {
    // 1. Alloc Output Context
    if (const int err =
                avformat_alloc_output_context2(&fmt_ctx_, nullptr, "webm", filename_.c_str());
        err < 0 || !fmt_ctx_) {
        return FailInit("Failed to create output context", err);
    }

    // 2. Find Encoder (VP9)
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
    if (!codec) {
        return FailInit("VP9 Codec not found");
    }

    // 3. Add Stream
    stream_ = avformat_new_stream(fmt_ctx_, codec);
    if (!stream_) {
        return FailInit("Failed to allocate stream");
    }
    stream_->id = static_cast<int>(fmt_ctx_->nb_streams - 1);

    // 4. Alloc Codec Context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        return FailInit("Failed to alloc codec context");
    }

    // 5. Set Parameters
    // We are intentionally downscaling the resolution by half to reduce the
    // encoding bitrate and computational overhead, which is beneficial for
    // real-time streaming or recording. The sws_scale will handle the
    // downscaling from the input RGB frame size to this codec context size.
    codec_ctx_->width = width_ / 2;
    codec_ctx_->height = height_ / 2;
    codec_ctx_->time_base = {.num = 1, .den = fps_};
    codec_ctx_->framerate = {.num = fps_, .den = 1};
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx_->bit_rate = bitrate_;
    codec_ctx_->gop_size = 10;

    // VP9 Speed Settings
    av_opt_set(codec_ctx_->priv_data, "deadline", "realtime", 0);
    av_opt_set(codec_ctx_->priv_data, "cpu-used", "4", 0);

    // 6. Open Codec
    if (const int err = avcodec_open2(codec_ctx_, codec, nullptr); err < 0) {
        return FailInit("Failed to open codec", err);
    }
    avcodec_parameters_from_context(stream_->codecpar, codec_ctx_);

    // 7. Open Output File
    if (const int err = avio_open(&fmt_ctx_->pb, filename_.c_str(), AVIO_FLAG_WRITE); err < 0) {
        return FailInit("Failed to open output file", err);
    }

    // 8. Write Header
    if (const int err = avformat_write_header(fmt_ctx_, nullptr); err < 0) {
        return FailInit("Failed to write header", err);
    }

    // 9. Allocate Frames & Packet
    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!frame_ || !pkt_) {
        return FailInit("Failed to alloc frame/packet");
    }

    frame_->format = codec_ctx_->pix_fmt;
    frame_->width = width_;
    frame_->height = height_;
    if (const int err = av_frame_get_buffer(frame_, 32); err < 0) {
        return FailInit("Failed to alloc frame buffer", err);
    }

    // 10. Init SwsContext (RGB -> YUV)
    sws_ctx_ = sws_getContext(width_, height_, AV_PIX_FMT_RGB24, width_ / 2, height_ / 2,
                              AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        return FailInit("Failed to init sws_ctx");
    }

    // 11. Start Thread
    is_initialized_ = true;
    encoder_thread_ = std::thread(&WebMEncoder::EncodeLoop, this);

    return true;
}

void WebMEncoder::AddFrame(const uint8_t* rgb_data) {
    TRACE_EVENT("display", "WebMEncoder::AddFrame");
    if (!rgb_data) return;

    std::vector<uint8_t> frame_data;
    {
        const absl::MutexLock lock(queue_mutex_);
        if (!is_initialized_ || stop_signal_) return;
        if (frame_queue_.size() >= kMaxQueuedFrames) {
            frame_data = std::move(frame_queue_.front());
            frame_queue_.pop_front();
        } else if (!free_buffers_.empty()) {
            frame_data = std::move(free_buffers_.back());
            free_buffers_.pop_back();
        }
    }

    const size_t data_size = static_cast<size_t>(width_) * height_ * kRgbChannels;
    if (frame_data.capacity() < data_size) {
        frame_buffer_allocations_.fetch_add(1, std::memory_order_relaxed);
    }
    frame_data.assign(rgb_data, rgb_data + data_size);

    {
        const absl::MutexLock lock(queue_mutex_);
        if (!is_initialized_ || stop_signal_) return;
        frame_queue_.push_back(std::move(frame_data));
        TRACE_COUNTER("display", "webm_queue_depth", frame_queue_.size());
    }
}

void WebMEncoder::Finish() {
    if (!is_initialized_ || finished_) return;

    // Signal thread
    {
        const absl::MutexLock lock(queue_mutex_);
        stop_signal_ = true;
    }

    VLOG(2) << " WebMEncoder: Finish wait for encoder_thread_ ";
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    VLOG(2) << " WebMEncoder: done Finish wait for encoder_thread_ ";
    finished_ = true;
}

size_t WebMEncoder::QueuedFrames() const {
    const absl::MutexLock lock(queue_mutex_);
    return frame_queue_.size();
}

size_t WebMEncoder::BufferedBytes() const {
    const absl::MutexLock lock(queue_mutex_);
    size_t total = in_flight_buffer_bytes_;
    for (const auto& buf : frame_queue_) {
        total += buf.capacity();
    }
    for (const auto& buf : free_buffers_) {
        total += buf.capacity();
    }
    return total;
}

bool WebMEncoder::IsFrameAvailable() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(queue_mutex_) {
    return !frame_queue_.empty() || stop_signal_;
}

void WebMEncoder::EncodeLoop() {
    int count = 0;
    std::vector<uint8_t> current_data;
    while (true) {
        ++count;
        VLOG(2) << "loop " << count;

        {
            const absl::MutexLock lock(queue_mutex_);
            if (!current_data.empty() && free_buffers_.size() <= kMaxQueuedFrames) {
                free_buffers_.push_back(std::move(current_data));
            }
            current_data = {};
            in_flight_buffer_bytes_ = 0;
            VLOG(2) << "loop " << count << " wait for new frame ";
            queue_mutex_.Await(absl::Condition(this, &WebMEncoder::IsFrameAvailable));

            VLOG(2) << "loop " << count << " got new frame ";
            if (stop_signal_) {
                // currently, just try to wrap up and return as quickly as possible
                // TODO: Consider processing the remaining frames in frame_queue_
                // before exiting to ensure all enqueued frames are encoded.
                VLOG(2) << "loop " << count << " exit now " << " stopSignal "
                        << " there are frames left: " << frame_queue_.size();
                frame_queue_.clear();
                frame_queue_.shrink_to_fit();
                free_buffers_.clear();
                free_buffers_.shrink_to_fit();
                break;
            }
            current_data = std::move(frame_queue_.front());
            frame_queue_.pop_front();
            in_flight_buffer_bytes_ = current_data.capacity();
            TRACE_COUNTER("display", "webm_queue_depth", frame_queue_.size());
        }

        ProcessFrame(current_data, pts_counter_++);
    }

    // Flush
    VLOG(2) << "send null frame to end encoder";
    avcodec_send_frame(codec_ctx_, nullptr);
    DrainPackets();

    av_write_trailer(fmt_ctx_);

    VLOG(2) << "finally clean up encoder context";
    // Close file IO context
    if (fmt_ctx_ && !(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&fmt_ctx_->pb);
    }
    VLOG(2) << "done";
}

void WebMEncoder::ProcessFrame(const std::vector<uint8_t>& raw_data, int64_t frame_index) {
    TRACE_EVENT("display", "WebMEncoder::ProcessFrame", "frame_index", frame_index);
    if (av_frame_make_writable(frame_) < 0) return;

    const uint8_t* const src_slice[AV_NUM_DATA_POINTERS] = {raw_data.data()};
    const int src_stride[AV_NUM_DATA_POINTERS] = {width_ * 3};

    {
        TRACE_EVENT("display", "sws_scale");
        sws_scale(sws_ctx_, src_slice, src_stride, 0, height_, frame_->data, frame_->linesize);
    }

    frame_->pts = frame_index;

    {
        TRACE_EVENT("display", "avcodec_send_frame");
        if (avcodec_send_frame(codec_ctx_, frame_) < 0) return;
    }
    frames_encoded_.fetch_add(1);

    DrainPackets();
}

void WebMEncoder::DrainPackets() {
    while (true) {
        const int ret = avcodec_receive_packet(codec_ctx_, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG(WARNING) << "[Encoder] Failed to receive encoded packet for '" << filename_
                         << "': " << err_buf << " (" << ret << ")";
            break;
        }

        av_packet_rescale_ts(pkt_, codec_ctx_->time_base, stream_->time_base);
        pkt_->stream_index = stream_->index;
        av_interleaved_write_frame(fmt_ctx_, pkt_);
        av_packet_unref(pkt_);
    }
}

void WebMEncoder::Cleanup() {
    {
        const absl::MutexLock lock(queue_mutex_);
        frame_queue_.clear();
        frame_queue_.shrink_to_fit();
        free_buffers_.clear();
        free_buffers_.shrink_to_fit();
        in_flight_buffer_bytes_ = 0;
        is_initialized_ = false;
    }
    av_frame_free(&frame_);
    av_packet_free(&pkt_);
    avcodec_free_context(&codec_ctx_);
    if (fmt_ctx_) {
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
}
}  // namespace goldfish::display
