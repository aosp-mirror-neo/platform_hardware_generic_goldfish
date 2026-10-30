/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "raw_video_file_source.h"

#include <libavutil/error.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"

#include "VerLogging.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}  // extern "C"

namespace android {
namespace ver {

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr bool kIsThreadSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kIsThreadSanitizer = true;
#else
constexpr bool kIsThreadSanitizer = false;
#endif
#else
constexpr bool kIsThreadSanitizer = false;
#endif

constexpr int kAutoDecodeThreads = 0;
constexpr int kSingleDecodeThread = 1;

int getVideoStreamIndex(const AVFormatContext& fmtctx) {
    for (unsigned i = 0; i < fmtctx.nb_streams; ++i) {
        if (fmtctx.streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return i;
        }
    }

    return -1;
}

int getVideoStreamRotation(const AVFormatContext& fmtctx, int streamIndex) {
    const AVPacketSideData* side_data = av_packet_side_data_get(
            fmtctx.streams[streamIndex]->codecpar->coded_side_data,
            fmtctx.streams[streamIndex]->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    const uint8_t* display_matrix = side_data ? side_data->data : nullptr;
    size_t display_matrix_size = side_data ? side_data->size : 0;

    double raw_rotation = 0;

    // Handle rotations in the modern display matrix way
    if (display_matrix && display_matrix_size >= 9 * sizeof(int32_t)) {
        // Currently ignoring the possibilities of flips
        raw_rotation = av_display_rotation_get(reinterpret_cast<const int32_t*>(display_matrix));

        // av_display_rotation_get returns in [-180.0, 180.0], or NaN
        if (std::isnan(raw_rotation)) {
            raw_rotation = 0;
        }
    } else {
        // Handle rotations via stream metadata
        AVDictionaryEntry* tag =
                av_dict_get(fmtctx.streams[streamIndex]->metadata, "rotate", nullptr, 0);
        if (!tag) {
            // If that's not present, check container metadata
            tag = av_dict_get(fmtctx.metadata, "rotate", nullptr, 0);
        }

        if (tag) {
            raw_rotation = atof(tag->value);
            if (std::isnan(raw_rotation)) {
                raw_rotation = 0;
            }
        } else {
            return 0;
        }
    }

    // We only care about 90 degree intervals
    int rotation = static_cast<int>(std::round((raw_rotation) / 90.0) * 90.0);

    rotation %= 360;
    if (rotation < 0) {
        rotation += 360;
    }
    return rotation;
}
}  // namespace

std::optional<RawVideofileSource::VideoFile> RawVideofileSource::openVideoFile(
        const char* filename) {
    int err;

    AVFormatContext* fmtCtxWeak = nullptr;
    err = avformat_open_input(&fmtCtxWeak, filename, nullptr, nullptr);
    if (err < 0) {
        derror("Could not open the '%s' file.", filename);
        return std::nullopt;
    }
    AVFormatContextPtr fmtCtx(fmtCtxWeak);

    err = avformat_find_stream_info(fmtCtx.get(), nullptr);
    if (err < 0) {
        derror("avformat_find_stream_info failed with %d for '%s'", err, filename);
        return std::nullopt;
    }

    const int videoStreamIndex = getVideoStreamIndex(*fmtCtx);
    if (videoStreamIndex < 0) {
        derror("Can't find the video stream in '%s'", filename);
        return std::nullopt;
    }

    const AVCodecParameters* codecParams = fmtCtx->streams[videoStreamIndex]->codecpar;
    const AVCodec* codec = ::avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        derror("Unsupported codec_id=%d in '%s'", codecParams->codec_id, filename);
        return std::nullopt;
    }

    AVCodecContext* codecCtxWeak = ::avcodec_alloc_context3(codec);
    if (!codecCtxWeak) {
        derror("avcodec_alloc_context3 failed for codec_id=%d for '%s'", codecParams->codec_id,
               filename);
        return std::nullopt;
    }

    err = ::avcodec_parameters_to_context(codecCtxWeak, codecParams);
    if (err < 0) {
        derror("avcodec_parameters_to_context failed with %d for '%s'", err, filename);
        ::avcodec_free_context(&codecCtxWeak);
        return std::nullopt;
    }

    if constexpr (kIsThreadSanitizer) {
        // @ffmpeg mixes C pthread_frame H.264 decoding with uninstrumented NASM
        // assembly, producing false-positive TSAN races under FF_THREAD_FRAME.
        codecCtxWeak->thread_count = kSingleDecodeThread;
    } else {
        // Set up multithreading. ffmpeg will determine the thread count.
        codecCtxWeak->thread_count = kAutoDecodeThreads;
        codecCtxWeak->thread_type = FF_THREAD_FRAME;
    }

    err = avcodec_open2(codecCtxWeak, codec, nullptr);
    if (err < 0) {
        derror("Could not open the codec (id=%d) for '%s'", err, filename);
        ::avcodec_free_context(&codecCtxWeak);
        return std::nullopt;
    }

    if (codecCtxWeak->thread_type == FF_THREAD_FRAME &&
        codecCtxWeak->active_thread_type != FF_THREAD_FRAME) {
        dwarning("%s: Video codec is not using FF_THREAD_FRAME", __func__);
    }

    AVCodecContextPtr codecCtx(codecCtxWeak);

    VideoFile videoFile;

    videoFile.baseRotation = getVideoStreamRotation(*fmtCtx, videoStreamIndex);
    videoFile.formatCtx = std::move(fmtCtx);
    videoFile.codecCtx = std::move(codecCtx);
    videoFile.videoStreamIndex = videoStreamIndex;

    return videoFile;
}

void RawVideofileSource::AVFormatContextDeleter::operator()(AVFormatContext* p) const {
    ::avformat_close_input(&p);
}

void RawVideofileSource::AVCodecContextDeleter::operator()(AVCodecContext* p) const {
    ::avcodec_free_context(&p);
}

void RawVideofileSource::AVFrameDeleter::operator()(AVFrame* p) const {
    ::av_frame_free(&p);
}

void RawVideofileSource::SwsContextDeleter::operator()(SwsContext* p) const {
    ::sws_freeContext(p);
}

RawVideofileSource::~RawVideofileSource() = default;

RawVideofileSource::RawVideofileSource(RawVideofileSource::VideoFile videoFile)
        : mVideoFile(std::move(videoFile)) {
    // Assuming 30 fps for the moment
    us_per_frame_ = 33333;
    seek_threshold_pts_ = this->GetPtsFromUs(us_per_frame_) * 10;
    last_update_time_us_ = 0;
    last_update_time_pts_ = 0;
    paused_ = false;
}

std::unique_ptr<RawVideofileSource> RawVideofileSource::Create(const std::string& filename) {
    std::optional<VideoFile> maybeVideofile = openVideoFile(filename.c_str());
    if (maybeVideofile) {
        return std::unique_ptr<RawVideofileSource>(
                new RawVideofileSource(std::move(maybeVideofile.value())));
    } else {
        return nullptr;
    }
}

int RawVideofileSource::Start(VerImageFormat pixel_format, int width, int height) {
    const int err = ::av_seek_frame(mVideoFile.formatCtx.get(), -1, 0, AVSEEK_FLAG_BACKWARD);
    if (err >= 0) {
        mConverterCache.clear();
        mFrameCache.reset(::av_frame_alloc());
        mConvertedFrameCache.reset(::av_frame_alloc());
        return (mFrameCache || mConvertedFrameCache) ? 0 : -1;
    } else {
        derror("av_seek_frame: err=%d", err);
        return err;
    }
};

absl::StatusOr<std::optional<RawImageToken>> RawVideofileSource::UpdateImage(
        int64_t target_time_us, std::optional<RawImageToken> token,
        std::function<absl::Status(const RawImageBufferView*)> updater) {
    if (!mFrameCache || !mConvertedFrameCache) {
        return absl::FailedPreconditionError("Video source is not started");
    }
    int64_t target_time_pts = GetPtsFromUs(target_time_us);
    // If the time has not changed, we're probably paused
    paused_ = (last_update_time_us_ == target_time_us);

    // If our requested target time has gone backwards, or is suitably far
    // ahead, seek
    if (target_time_us < last_update_time_us_ ||
        target_time_pts - last_update_time_pts_ > seek_threshold_pts_) {
        int err = ::av_seek_frame(mVideoFile.formatCtx.get(), mVideoFile.videoStreamIndex,
                                  target_time_pts, AVSEEK_FLAG_BACKWARD);
        if (err < 0) {
            return absl::UnavailableError(
                    absl::StrFormat("%s:%d av_seek_frame: err=%d", __func__, __LINE__, err));
        }
        avcodec_flush_buffers(mVideoFile.codecCtx.get());
    } else if (last_update_time_pts_ > target_time_pts) {
        // We're actually already ahead, nothing to do.
        return std::nullopt;
    }
    last_update_time_us_ = target_time_us;
    // If we're paused and up to date, we don't need to do anything.
    // Otherwise the requestor either has no current image, or has
    // an image that is not up to date with what we're providing
    if (paused_ && token.has_value() && token.value().token == last_update_time_pts_) {
        return std::nullopt;
    }

    int err = 0;
    int max_skip = 100;
    int64_t last_frame_pts;
    int64_t curr_frame_pts = last_update_time_pts_;
    do {
        last_frame_pts = curr_frame_pts;
        err = decodeNextFrame();
        if (err < 0) {
            break;
        }
        curr_frame_pts = mFrameCache->best_effort_timestamp;
        if (mFrameCache->best_effort_timestamp > target_time_pts ||
            mFrameCache->best_effort_timestamp == AV_NOPTS_VALUE) {
            break;
        }
        // Skip frames to catch up. If our pts goes backwards, we probably
        // looped the video, and should give up
        max_skip--;
        dprint("%s: Video source dropping frame at %.3f", __func__,
               GetUsFromPts(mFrameCache->best_effort_timestamp) / 1000000.0);
    } while (!err && max_skip > 0 && curr_frame_pts > last_frame_pts);

    if (err == AVERROR_EOF) {
        return std::nullopt;
    }
    if (err >= 0) {
        err = convertFrameToRGBA();
        if (err < 0) {
            return absl::UnavailableError(absl::StrFormat("Could not convert the frame: %d", err));
        }

        const AVFrame* avFrame = mConvertedFrameCache.get();
        RawImageBufferView img;
        img.buffer = avFrame->data[0];
        img.buffer_size = avFrame->width * avFrame->height * 4;
        img.width = avFrame->width;
        img.height = avFrame->height;
        img.pixel_format = VerImageFormat::RGBA8;

        int64_t frame_time;
        if (avFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
            frame_time = avFrame->best_effort_timestamp;
        } else {
            dwarning("No best effort time available. Using provided time of %" PRId64,
                     last_update_time_us_);
            frame_time = target_time_us;
        }
        absl::Status ret = updater(&img);
        if (ret.ok()) {
            last_update_time_pts_ = frame_time;
            return RawImageToken{frame_time};
        } else {
            return ret;
        }
    } else {
        return absl::UnavailableError(absl::StrFormat("Could not decode the frame: %d", err));
    }
}

int RawVideofileSource::Stop() {
    mFrameCache.reset();
    mConvertedFrameCache.reset();
    mConverterCache.clear();
    return 0;
};

int RawVideofileSource::GetBaseRotation() {
    return mVideoFile.baseRotation;
}

int64_t RawVideofileSource::GetAnimationLengthUs() {
    if (mVideoFile.formatCtx->duration == AV_NOPTS_VALUE) {
        return 0;
    }
    return mVideoFile.formatCtx->duration;
}

// Converts from the stream specific pts units to a microsecond value
int64_t RawVideofileSource::GetUsFromPts(int64_t pts) const {
    AVRational stream_time_base =
            mVideoFile.formatCtx->streams[mVideoFile.videoStreamIndex]->time_base;

    return av_rescale_q(pts, stream_time_base, AV_TIME_BASE_Q);
};

// Converts from the stream specific pts units to a microsecond value
int64_t RawVideofileSource::GetPtsFromUs(int64_t us) const {
    AVRational stream_time_base =
            mVideoFile.formatCtx->streams[mVideoFile.videoStreamIndex]->time_base;

    return av_rescale_q(us, AV_TIME_BASE_Q, stream_time_base);
};

int RawVideofileSource::decodeNextFrame() {
    int err;

    // First, try to receive a frame that might already be decoded.
    // This is important because a single packet can contain multiple frames,
    // or the decoder might have buffered frames.
    err = ::avcodec_receive_frame(mVideoFile.codecCtx.get(), mFrameCache.get());
    if (err >= 0) {
        return 0;
    } else if (err == AVERROR_EOF) {
        return err;
    } else if (err != AVERROR(EAGAIN)) {
        derror("avcodec_receive_frame: err=%d", err);
        return err;
    }

    AVPacket packet;
    av_init_packet(&packet);
    packet.data = nullptr;
    packet.size = 0;

    while ((err = ::av_read_frame(mVideoFile.formatCtx.get(), &packet)) >= 0) {
        if (packet.stream_index != mVideoFile.videoStreamIndex) {
            ::av_packet_unref(&packet);
            continue;
        }

        err = ::avcodec_send_packet(mVideoFile.codecCtx.get(), &packet);
        ::av_packet_unref(&packet);

        if (err < 0) {
            derror("avcodec_send_packet: err=%d", err);
            continue;
        }

        err = ::avcodec_receive_frame(mVideoFile.codecCtx.get(), mFrameCache.get());
        if (err >= 0) {
            return 0;
        } else if (err != AVERROR(EAGAIN)) {
            derror("avcodec_receive_frame: err=%d", err);
        }
    }

    // When we hit EOF, we must drain the remainder of the frames to actually
    // decode to the end of the file Subsequent calls will be caught by the
    // avcodec_receive_frame at the top of the function
    if (err == AVERROR_EOF) {
        avcodec_send_packet(mVideoFile.codecCtx.get(), nullptr);
        err = ::avcodec_receive_frame(mVideoFile.codecCtx.get(), mFrameCache.get());
        if (err >= 0) {
            return 0;
        } else if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
            derror("avcodec_receive_frame: err=%d", err);
            return err;
        }
    }
    return err;
}

int RawVideofileSource::convertFrameToRGBA() {
    SwsContext* swsCtx = getSwsContext(mFrameCache->width, mFrameCache->height,
                                       static_cast<AVPixelFormat>(mFrameCache->format),
                                       mFrameCache->width, mFrameCache->height, AV_PIX_FMT_RGBA);
    if (!swsCtx) {
        derror("Could not allocate SwsContext for src={ %dx%d, fmt=%d }, "
               "dst={ %dx%d, fmt=%d }",
               mFrameCache->width, mFrameCache->height, mFrameCache->format, mFrameCache->width,
               mFrameCache->height, AV_PIX_FMT_RGBA);
        return -ENOMEM;
    }
    if (!mConvertedFrameCache || mConvertedFrameCache->width != mFrameCache->width ||
        mConvertedFrameCache->height != mFrameCache->height) {
        mConvertedFrameCache.reset(::av_frame_alloc());
        if (!mConvertedFrameCache) {
            derror("Could not allocate converted frame");
            return -ENOMEM;
        }
        mConvertedFrameCache->format = AV_PIX_FMT_RGBA;
        mConvertedFrameCache->width = mFrameCache->width;
        mConvertedFrameCache->height = mFrameCache->height;
        int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGBA, mFrameCache->width,
                                                  mFrameCache->height, 1);
        mConvertedFrameBuffer.resize(bufferSize + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        int ret = av_image_fill_arrays(mConvertedFrameCache->data, mConvertedFrameCache->linesize,
                                       mConvertedFrameBuffer.data(), AV_PIX_FMT_RGBA,
                                       mFrameCache->width, mFrameCache->height, 1);
        if (ret < 0) {
            return ret;
        }
    }

    ::sws_scale(swsCtx, mFrameCache->data, mFrameCache->linesize, 0, mFrameCache->height,
                mConvertedFrameCache->data, mConvertedFrameCache->linesize);
    mConvertedFrameCache->best_effort_timestamp = mFrameCache->best_effort_timestamp;
    return 0;
}

SwsContext* RawVideofileSource::getSwsContext(const int srcWidth, const int srcHeight,
                                              const int srcFmt, const int dstWidth,
                                              const int dstHeight, const int dstFmt) {
    ConversionKey conv = {
        .srcWidth = srcWidth,
        .srcHeight = srcHeight,
        .srcFmt = srcFmt,
        .dstWidth = dstWidth,
        .dstHeight = dstHeight,
        .dstFmt = dstFmt,
    };

    const auto i = std::find_if(mConverterCache.begin(), mConverterCache.end(),
                                [&conv](const std::pair<ConversionKey, SwsContextPtr>& kv) {
                                    return conv == kv.first;
                                });
    if (i != mConverterCache.end()) {
        return i->second.get();
    }

    SwsContext* ctx = ::sws_getContext(srcWidth, srcHeight, static_cast<AVPixelFormat>(srcFmt),
                                       dstWidth, dstHeight, static_cast<AVPixelFormat>(dstFmt),
                                       SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (ctx) {
        mConverterCache.push_back({std::move(conv), SwsContextPtr(ctx)});
    }

    return ctx;
}

}  // namespace ver
}  // namespace android