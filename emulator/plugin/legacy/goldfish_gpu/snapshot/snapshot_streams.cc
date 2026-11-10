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

#include "goldfish/legacy/goldfish_gpu/snapshot/snapshot_streams.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <ios>
#include <string_view>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"

namespace goldfish::legacy {
namespace {

// be64 end_pos + be32 count.
constexpr uint64_t kTextureFooterHeaderSize = 12;
// be32 tex_id + be64 offset.
constexpr uint64_t kTextureIndexEntrySize = 12;
// Trailing be32 footer_size.
constexpr int64_t kTextureFooterSizeFieldSize = 4;

}  // namespace

// ---------------------------------------------------------------------------
// FileStream

FileStream::FileStream(const std::filesystem::path& path, std::ios::openmode mode)
        : path_(path), stream_(path, mode | std::ios::binary) {}

ssize_t FileStream::read(void* buffer, size_t size) {
    if (!stream_.is_open() || size == 0) {
        return 0;
    }
    stream_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return static_cast<ssize_t>(stream_.gcount());
}

ssize_t FileStream::write(const void* buffer, size_t size) {
    if (!stream_.is_open() || size == 0) {
        return 0;
    }
    stream_.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
    return stream_.good() ? static_cast<ssize_t>(size) : -1;
}

int64_t FileStream::TellWrite() {
    return stream_.is_open() ? static_cast<int64_t>(stream_.tellp()) : -1;
}

int64_t FileStream::FileSize() {
    if (!stream_.is_open()) {
        return -1;
    }
    stream_.clear();
    stream_.seekg(0, std::ios::end);
    if (stream_.fail()) {
        return -1;
    }
    return static_cast<int64_t>(stream_.tellg());
}

bool FileStream::SeekRead(int64_t pos) {
    if (!stream_.is_open() || pos < 0) {
        return false;
    }
    stream_.clear();
    stream_.seekg(pos, std::ios::beg);
    return !stream_.fail();
}

void FileStream::Flush() {
    if (stream_.is_open()) {
        stream_.flush();
    }
}

// ---------------------------------------------------------------------------
// BufferStream

BufferStream::BufferStream(std::vector<uint8_t>* write_buf) : write_buf_(write_buf) {}

BufferStream::BufferStream(const uint8_t* read_data, size_t read_size)
        : read_data_(read_data),
          read_size_(read_size),
          read_mode_(true),
          read_error_(read_data == nullptr && read_size > 0) {}

ssize_t BufferStream::read(void* buffer, size_t size) {
    if (!read_mode_ || size == 0) {
        return 0;
    }
    // A short read is always an error for the callers (getBe32(), getString(),
    // ...), so do not hand out a partial result: fail, latch, and let every
    // later read fail too.
    if (read_error_ || size > read_size_ - read_pos_) {
        read_error_ = true;
        return 0;
    }
    if (buffer != nullptr) {
        std::memcpy(buffer, read_data_ + read_pos_, size);
    }
    read_pos_ += size;
    return static_cast<ssize_t>(size);
}

ssize_t BufferStream::write(const void* buffer, size_t size) {
    if (write_buf_ == nullptr) {
        return -1;
    }
    if (size > 0) {
        CHECK(buffer != nullptr) << "BufferStream::write: null buffer with size=" << size;
        const auto* bytes = static_cast<const uint8_t*>(buffer);
        write_buf_->insert(write_buf_->end(), bytes, bytes + size);
    }
    return static_cast<ssize_t>(size);
}

bool BufferStream::hasErrors() const {
    return StreamWithErrorLogger::hasErrors() || (read_mode_ && read_error_);
}

size_t BufferStream::Remaining() const {
    return read_mode_ ? (read_size_ - read_pos_) : 0;
}

absl::Status BufferStream::ReadStatus(std::string_view what) const {
    if (!read_mode_ || !read_error_) {
        return absl::OkStatus();
    }
    return absl::DataLossError(absl::StrCat("truncated ", what, " at byte ", read_pos_, " of ",
                                            read_size_));
}

// ---------------------------------------------------------------------------
// FileTextureSaver

FileTextureSaver::FileTextureSaver(const std::filesystem::path& path)
        : stream_(path, std::ios::out | std::ios::trunc) {}

void FileTextureSaver::saveTexture(uint32_t tex_id, const saver_t& saver) {
    if (!stream_.IsValid()) {
        ok_ = false;
        return;
    }
    const int64_t pos = stream_.TellWrite();
    if (pos < 0) {
        ok_ = false;
        return;
    }
    index_[tex_id] = pos;
    saver(&stream_, nullptr);
}

absl::Status FileTextureSaver::Done() {
    const std::string path = stream_.Path().string();
    if (!stream_.IsValid() || !ok_ || stream_.hasErrors()) {
        return absl::DataLossError(
                absl::StrCat("failed to write texture data to '", path, "'",
                             stream_.hasErrors() ? absl::StrCat(": ", *stream_.getErrors()) : ""));
    }
    const int64_t end_pos = stream_.TellWrite();
    if (end_pos < 0) {
        return absl::DataLossError(absl::StrCat("tellp() failed on '", path, "'"));
    }
    stream_.putBe64(static_cast<uint64_t>(end_pos));
    stream_.putBe32(static_cast<uint32_t>(index_.size()));
    for (const auto& [tex_id, offset] : index_) {
        stream_.putBe32(tex_id);
        stream_.putBe64(static_cast<uint64_t>(offset));
    }
    const int64_t footer_end = stream_.TellWrite();
    if (footer_end < end_pos) {
        return absl::DataLossError(absl::StrCat("tellp() went backwards on '", path, "'"));
    }
    stream_.putBe32(static_cast<uint32_t>(footer_end - end_pos));
    stream_.Flush();
    if (!stream_.IsValid() || stream_.hasErrors()) {
        return absl::DataLossError(
                absl::StrCat("failed to write texture index footer to '", path, "'",
                             stream_.hasErrors() ? absl::StrCat(": ", *stream_.getErrors()) : ""));
    }
    return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// FileTextureLoader

FileTextureLoader::FileTextureLoader(const std::filesystem::path& path)
        : stream_(path, std::ios::in) {}

absl::Status FileTextureLoader::Start() {
    absl::MutexLock lock(&mu_);
    const std::string path = stream_.Path().string();
    if (!stream_.IsValid()) {
        return absl::DataLossError(absl::StrCat("failed to open texture file '", path, "'"));
    }
    const int64_t file_size = stream_.FileSize();
    const int64_t min_file_size =
            static_cast<int64_t>(kTextureFooterHeaderSize) + kTextureFooterSizeFieldSize;
    if (file_size < min_file_size) {
        return absl::DataLossError(absl::StrCat("texture file '", path, "' is ", file_size,
                                                " bytes, shorter than the ", min_file_size,
                                                "-byte footer"));
    }
    if (!stream_.SeekRead(file_size - kTextureFooterSizeFieldSize)) {
        return absl::DataLossError(
                absl::StrCat("seek to footer size of texture file '", path, "' failed"));
    }
    const uint32_t footer_size = stream_.getBe32();
    if (stream_.hasErrors()) {
        return absl::DataLossError(
                absl::StrCat("failed to read footer size of texture file '", path, "'"));
    }
    if (footer_size < kTextureFooterHeaderSize ||
        static_cast<int64_t>(footer_size) + kTextureFooterSizeFieldSize > file_size) {
        return absl::DataLossError(absl::StrCat(
                "texture file '", path, "' footer_size ", footer_size, " is outside [",
                kTextureFooterHeaderSize, ", ", file_size - kTextureFooterSizeFieldSize, "]"));
    }
    const int64_t footer_pos = file_size - kTextureFooterSizeFieldSize - footer_size;
    if (!stream_.SeekRead(footer_pos)) {
        return absl::DataLossError(
                absl::StrCat("seek to footer of texture file '", path, "' failed"));
    }
    const auto end_pos = static_cast<int64_t>(stream_.getBe64());
    const uint32_t count = stream_.getBe32();
    if (stream_.hasErrors()) {
        return absl::DataLossError(
                absl::StrCat("failed to read footer header of texture file '", path, "'"));
    }
    // Exact: FileTextureSaver::Done() writes 12 bytes of header plus 12 bytes
    // per entry, so any other footer_size means the count or the file is bad.
    const uint64_t expected_footer_size =
            kTextureFooterHeaderSize + kTextureIndexEntrySize * uint64_t{count};
    if (footer_size != expected_footer_size) {
        return absl::DataLossError(absl::StrCat(
                "texture file '", path, "' footer_size ", footer_size, " does not match ",
                expected_footer_size, " for texture count ", count));
    }
    if (end_pos != footer_pos) {
        return absl::DataLossError(absl::StrCat("texture file '", path, "' footer end_pos ",
                                                end_pos, " does not match footer offset ",
                                                footer_pos));
    }
    index_.clear();
    index_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t tex_id = stream_.getBe32();
        const auto offset = static_cast<int64_t>(stream_.getBe64());
        if (stream_.hasErrors()) {
            return absl::DataLossError(absl::StrCat(
                    "texture file '", path, "' index truncated at entry ", i, " of ", count));
        }
        if (offset < 0 || offset > end_pos) {
            return absl::DataLossError(absl::StrCat("texture file '", path, "' texture ", tex_id,
                                                    " offset ", offset, " is outside the ", end_pos,
                                                    "-byte data region"));
        }
        index_[tex_id] = offset;
    }
    end_pos_ = end_pos;
    if (!stream_.SeekRead(0)) {
        return absl::DataLossError(absl::StrCat("rewind of texture file '", path, "' failed"));
    }
    return absl::OkStatus();
}

size_t FileTextureLoader::TextureCount() const {
    absl::MutexLock lock(&mu_);
    return index_.size();
}

void FileTextureLoader::loadTexture(uint32_t tex_id, const loader_t& loader) {
    absl::MutexLock lock(&mu_);
    const auto it = index_.find(tex_id);
    if (it == index_.end()) {
        return;
    }
    if (!stream_.SeekRead(it->second)) {
        return;
    }
    loader(&stream_);
}

}  // namespace goldfish::legacy
