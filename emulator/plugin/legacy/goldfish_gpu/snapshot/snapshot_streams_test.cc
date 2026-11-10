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

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "absl/status/status.h"

#include "android/base/testing/test_temp_dir.h"
#include "android/status/status_matcher_macros.h"
#include "render-utils/stream.h"

namespace goldfish::legacy {
namespace {

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void PutBe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
    (*bytes)[offset] = static_cast<uint8_t>(value >> 24);
    (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 16);
    (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 8);
    (*bytes)[offset + 3] = static_cast<uint8_t>(value);
}

class SnapshotStreamsTest : public ::testing::Test {
  protected:
    // Writes two textures (ids 7 and 9) with FileTextureSaver and returns the
    // file bytes. Layout: [TEX7 payload 4B][TEX9 payload 8B][footer 36B][be32 36].
    std::vector<uint8_t> WriteGoodTextureFile() {
        FileTextureSaver saver(texture_path_);
        saver.saveTexture(7, [](gfxstream::Stream* s, gfxstream::ITextureSaver::Buffer*) {
            s->putBe32(0x54455837);  // "TEX7"
        });
        saver.saveTexture(9, [](gfxstream::Stream* s, gfxstream::ITextureSaver::Buffer*) {
            s->putBe64(0x5445583900000009ULL);
        });
        EXPECT_OK(saver.Done());
        return ReadFileBytes(texture_path_);
    }

    android::base::TestTempDir temp_dir_{"snapshot_streams"};
    // Non-ASCII path component per AGENTS.md.
    std::filesystem::path texture_path_ = temp_dir_.path / "测试_🚀_é_textures.bin";
};

TEST_F(SnapshotStreamsTest, BufferStreamRoundTripsBigEndianValues) {
    std::vector<uint8_t> bytes;
    BufferStream out(&bytes);
    out.putBe32(0x01020304);
    out.putBe64(0x1112131415161718ULL);
    out.putByte(0xAB);
    EXPECT_FALSE(out.hasErrors());
    ASSERT_EQ(bytes.size(), 13U);
    EXPECT_EQ(bytes[0], 0x01);
    EXPECT_EQ(bytes[3], 0x04);
    EXPECT_EQ(bytes[4], 0x11);
    EXPECT_EQ(bytes[11], 0x18);
    EXPECT_EQ(bytes[12], 0xAB);

    BufferStream in(bytes.data(), bytes.size());
    EXPECT_EQ(in.Remaining(), 13U);
    EXPECT_EQ(in.getBe32(), 0x01020304U);
    EXPECT_EQ(in.getBe64(), 0x1112131415161718ULL);
    EXPECT_EQ(in.getByte(), 0xAB);
    EXPECT_EQ(in.Remaining(), 0U);
    EXPECT_FALSE(in.hasErrors());
    EXPECT_OK(in.ReadStatus("payload"));
}

TEST_F(SnapshotStreamsTest, BufferStreamReadPastEndLatchesError) {
    const std::vector<uint8_t> bytes = {0, 0, 0, 5, 0, 0};
    BufferStream in(bytes.data(), bytes.size());
    EXPECT_EQ(in.getBe32(), 5U);
    EXPECT_EQ(in.getBe32(), 0U);  // Only two bytes left: short read, no partial result.
    EXPECT_TRUE(in.hasErrors());
    const absl::Status st = in.ReadStatus("counter");
    EXPECT_EQ(st.code(), absl::StatusCode::kDataLoss);
    EXPECT_NE(st.message().find("counter"), std::string::npos);
    // Every later read fails too, even one that would otherwise fit.
    EXPECT_EQ(in.getByte(), 0U);
    EXPECT_EQ(in.Remaining(), 2U);
}

TEST_F(SnapshotStreamsTest, TextureLoaderStartsOnGoodFile) {
    const std::vector<uint8_t> bytes = WriteGoodTextureFile();
    // 4 + 8 bytes of texture data, 12 + 2 * 12 bytes of footer, 4 bytes of footer size.
    ASSERT_EQ(bytes.size(), 12U + 36U + 4U);

    FileTextureLoader loader(texture_path_);
    ASSERT_OK(loader.Start());
    EXPECT_TRUE(loader.start());  // Idempotent: gfxstream calls start() again.
    EXPECT_EQ(loader.TextureCount(), 2U);

    uint32_t tex7 = 0;
    loader.loadTexture(7, [&tex7](gfxstream::Stream* s) { tex7 = s->getBe32(); });
    EXPECT_EQ(tex7, 0x54455837U);
    uint64_t tex9 = 0;
    loader.loadTexture(9, [&tex9](gfxstream::Stream* s) { tex9 = s->getBe64(); });
    EXPECT_EQ(tex9, 0x5445583900000009ULL);
    bool unknown_called = false;
    loader.loadTexture(8, [&unknown_called](gfxstream::Stream*) { unknown_called = true; });
    EXPECT_FALSE(unknown_called);
}

TEST_F(SnapshotStreamsTest, TextureLoaderRejectsMissingFile) {
    FileTextureLoader loader(temp_dir_.path / "does_not_exist.bin");
    const absl::Status st = loader.Start();
    EXPECT_EQ(st.code(), absl::StatusCode::kDataLoss);
    EXPECT_NE(st.message().find("does_not_exist.bin"), std::string::npos);
    EXPECT_FALSE(loader.start());
}

TEST_F(SnapshotStreamsTest, TextureLoaderRejectsEveryTruncation) {
    const std::vector<uint8_t> bytes = WriteGoodTextureFile();
    for (size_t n = 0; n < bytes.size(); ++n) {
        WriteFileBytes(texture_path_, std::vector<uint8_t>(bytes.begin(), bytes.begin() + n));
        FileTextureLoader loader(texture_path_);
        const absl::Status st = loader.Start();
        EXPECT_EQ(st.code(), absl::StatusCode::kDataLoss) << "prefix of " << n << " bytes: " << st;
        EXPECT_NE(st.message().find(texture_path_.string()), std::string::npos)
                << "prefix of " << n << " bytes: " << st;
    }
}

TEST_F(SnapshotStreamsTest, TextureLoaderRejectsHugeCount) {
    std::vector<uint8_t> bytes = WriteGoodTextureFile();
    // Footer starts after the 12 bytes of texture data: be64 end_pos, be32 count.
    const size_t count_offset = 12 + 8;
    PutBe32(&bytes, count_offset, 0xffffffff);
    WriteFileBytes(texture_path_, bytes);
    FileTextureLoader loader(texture_path_);
    const absl::Status st = loader.Start();
    EXPECT_EQ(st.code(), absl::StatusCode::kDataLoss);
    EXPECT_NE(st.message().find("4294967295"), std::string::npos) << st;
    EXPECT_EQ(loader.TextureCount(), 0U);
}

TEST_F(SnapshotStreamsTest, TextureLoaderRejectsFooterSizeMismatch) {
    std::vector<uint8_t> bytes = WriteGoodTextureFile();
    // Claim one more entry than the footer actually holds.
    PutBe32(&bytes, 12 + 8, 3);
    WriteFileBytes(texture_path_, bytes);
    EXPECT_EQ(FileTextureLoader(texture_path_).Start().code(), absl::StatusCode::kDataLoss);

    // footer_size pointing past the start of the file.
    bytes = WriteGoodTextureFile();
    PutBe32(&bytes, bytes.size() - 4, 0x7fffffff);
    WriteFileBytes(texture_path_, bytes);
    EXPECT_EQ(FileTextureLoader(texture_path_).Start().code(), absl::StatusCode::kDataLoss);

    // Texture offset outside the data region.
    bytes = WriteGoodTextureFile();
    PutBe32(&bytes, 12 + 12 + 4 + 4, 0x40000000);  // low half of the first entry's be64 offset
    WriteFileBytes(texture_path_, bytes);
    EXPECT_EQ(FileTextureLoader(texture_path_).Start().code(), absl::StatusCode::kDataLoss);
}

TEST_F(SnapshotStreamsTest, TextureSaverReportsUnwritableFile) {
    FileTextureSaver saver(temp_dir_.path / "missing_dir" / "textures.bin");
    saver.saveTexture(
            1, [](gfxstream::Stream* s, gfxstream::ITextureSaver::Buffer*) { s->putBe32(1); });
    const absl::Status st = saver.Done();
    EXPECT_EQ(st.code(), absl::StatusCode::kDataLoss);
    EXPECT_NE(st.message().find("textures.bin"), std::string::npos);
}

}  // namespace
}  // namespace goldfish::legacy
