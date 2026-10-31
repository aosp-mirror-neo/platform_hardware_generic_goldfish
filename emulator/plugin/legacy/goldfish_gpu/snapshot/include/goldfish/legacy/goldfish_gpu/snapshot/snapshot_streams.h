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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"

#include "render-utils/snapshot_operations.h"
#include "render-utils/stream.h"

// gfxstream::Stream adapters and texture saver/loader used by goldfish_gpu to
// persist the gfxstream renderer state of a snapshot:
//
//   <snapshot_dir>/<name>/gfxstream_render.bin    Renderer::save() stream
//   <snapshot_dir>/<name>/gfxstream_textures.bin  one record per texture plus
//                                                 a footer index (see
//                                                 FileTextureSaver::Done()).
//
// The device metadata handed to QEMU's VMState goes through BufferStream,
// which is a thin gfxstream::Stream facade over an in-memory byte buffer.

namespace goldfish::legacy {

// gfxstream::Stream backed by a file opened with std::fstream.
class FileStream final : public gfxstream::StreamWithErrorLogger {
  public:
    FileStream(const std::filesystem::path& path, std::ios::openmode mode);

    // True while the file is open and no stream error has been recorded.
    bool IsValid() const { return stream_.is_open() && !stream_.fail(); }
    const std::filesystem::path& Path() const { return path_; }

    ssize_t read(void* buffer, size_t size) override;
    ssize_t write(const void* buffer, size_t size) override;

    // Current write position, or -1 if the file is not open.
    int64_t TellWrite();
    // Total file size (moves the read position to the end), or -1 on error.
    int64_t FileSize();
    bool SeekRead(int64_t pos);
    void Flush();

  private:
    std::filesystem::path path_;
    std::fstream stream_;
};

// gfxstream::Stream over an in-memory byte buffer. In write mode every byte
// is appended to `*write_buf`; in read mode bytes are consumed from
// `[read_data, read_data + read_size)`, so a read past the end returns 0,
// latches the error and makes every later read fail too.
class BufferStream final : public gfxstream::StreamWithErrorLogger {
  public:
    explicit BufferStream(std::vector<uint8_t>* write_buf);
    BufferStream(const uint8_t* read_data, size_t read_size);

    ssize_t read(void* buffer, size_t size) override;
    ssize_t write(const void* buffer, size_t size) override;
    bool hasErrors() const override;

    // Unread bytes left in read mode (0 in write mode).
    size_t Remaining() const;
    // OK while no read ran past the end of the buffer, otherwise a DATA_LOSS
    // error naming `what` and the offset of the failed read.
    absl::Status ReadStatus(std::string_view what) const;

  private:
    std::vector<uint8_t>* write_buf_ = nullptr;
    const uint8_t* read_data_ = nullptr;
    size_t read_size_ = 0;
    size_t read_pos_ = 0;
    bool read_mode_ = false;
    bool read_error_ = false;
};

// Writes every texture handed to saveTexture() back to back into one file and
// finishes it with a footer:
//   be64 end_pos              offset of the footer (= size of the texture data)
//   be32 count
//   count x { be32 tex_id, be64 offset }
//   be32 footer_size          = 12 + 12 * count (the bytes before this field)
class FileTextureSaver final : public gfxstream::ITextureSaver {
  public:
    explicit FileTextureSaver(const std::filesystem::path& path);

    void saveTexture(uint32_t tex_id, const saver_t& saver) override;

    // Writes the footer and flushes. Returns a DATA_LOSS error naming the
    // file when any write failed.
    absl::Status Done();

  private:
    FileStream stream_;
    std::unordered_map<uint32_t, int64_t> index_;
    bool ok_ = true;
};

// Reads a file written by FileTextureSaver.
class FileTextureLoader final : public gfxstream::ITextureLoader {
  public:
    explicit FileTextureLoader(const std::filesystem::path& path);

    // Validates the footer and builds the texture index. Returns a
    // DATA_LOSS error naming the file when it is missing, truncated or its
    // footer is inconsistent. Safe to call more than once; gfxstream calls
    // `start()` itself, so the device calls `Start()` beforehand to turn a
    // corrupt file into a load failure instead of a GFXSTREAM_FATAL abort.
    absl::Status Start() ABSL_LOCKS_EXCLUDED(mu_);
    size_t TextureCount() const ABSL_LOCKS_EXCLUDED(mu_);

    bool start() override ABSL_LOCKS_EXCLUDED(mu_) { return Start().ok(); }
    void loadTexture(uint32_t tex_id, const loader_t& loader) override ABSL_LOCKS_EXCLUDED(mu_);

  private:
    mutable absl::Mutex mu_;
    FileStream stream_ ABSL_GUARDED_BY(mu_);
    std::unordered_map<uint32_t, int64_t> index_ ABSL_GUARDED_BY(mu_);
    int64_t end_pos_ ABSL_GUARDED_BY(mu_) = 0;
};

}  // namespace goldfish::legacy
