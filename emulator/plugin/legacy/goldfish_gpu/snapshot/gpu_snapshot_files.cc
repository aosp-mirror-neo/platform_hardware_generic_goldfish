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

#include "goldfish/legacy/goldfish_gpu/snapshot/gpu_snapshot_files.h"

#include <cstdint>
#include <filesystem>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "android/status/status_macros.h"

#include "goldfish/legacy/goldfish_gpu/snapshot/snapshot_streams.h"

namespace goldfish::legacy {
namespace {

std::string StreamErrors(const gfxstream::StreamWithErrorLogger& stream) {
    return stream.hasErrors() ? absl::StrCat(": ", *stream.getErrors()) : "";
}

// Drives ITextureSaver without keeping anything (in-memory mode).
class NoOpTextureSaver final : public gfxstream::ITextureSaver {
  public:
    void saveTexture(uint32_t /*tex_id*/, const saver_t& saver) override {
        std::vector<uint8_t> tex_buf;
        BufferStream s(&tex_buf);
        saver(&s, nullptr);
    }
};

}  // namespace

GpuSnapshotFiles::GpuSnapshotFiles(std::filesystem::path snapshot_dir)
    : snapshot_dir_(std::move(snapshot_dir)) {}

bool GpuSnapshotFiles::IsOnDisk(std::string_view snapshot_name) const {
    return !snapshot_dir_.empty() && !snapshot_name.empty();
}

std::filesystem::path GpuSnapshotFiles::Directory(std::string_view snapshot_name) const {
    return snapshot_dir_ / std::string(snapshot_name);
}

absl::Status GpuSnapshotFiles::ValidateName(std::string_view snapshot_name) {
    // ':' covers Windows drive-relative names ("C:x") and NTFS alternate data
    // streams ("x:stream"), both of which would escape or alias the directory.
    if (snapshot_name.empty() || snapshot_name == "." || snapshot_name == ".." ||
        snapshot_name.find_first_of("/\\:") != std::string_view::npos ||
        snapshot_name.find('\0') != std::string_view::npos) {
        return absl::InvalidArgumentError(absl::StrCat(
                "snapshot name '", snapshot_name,
                "' must be a single path component (no '/', '\\\\' or ':', not \".\" or \"..\")"));
    }
    return absl::OkStatus();
}

absl::Status GpuSnapshotFiles::Save(std::string_view snapshot_name,
                                    gfxstream::Renderer* renderer) const {
    if (!IsOnDisk(snapshot_name)) {
        std::vector<uint8_t> dummy_render;
        BufferStream render_stream(&dummy_render);
        if (!renderer->save(&render_stream, std::make_shared<NoOpTextureSaver>())) {
            return absl::InternalError(
                    "gfxstream Renderer::save() failed (in-memory save, no snapshot "
                    "directory configured)");
        }
        return absl::OkStatus();
    }

    RETURN_IF_ERROR(ValidateName(snapshot_name));
    const std::filesystem::path dir = Directory(snapshot_name);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return absl::InternalError(absl::StrCat("failed to create snapshot directory '",
                                                dir.string(), "': ", ec.message()));
    }
    const std::filesystem::path render_path = dir / kRenderStreamFileName;
    const std::filesystem::path texture_path = dir / kTextureFileName;
    absl::Status result = SaveFiles(render_path, texture_path, renderer);
    if (!result.ok()) {
        // Never leave a half-written pair behind: ArtifactsPresent() would
        // otherwise report the snapshot as complete.
        std::error_code ignored;
        std::filesystem::remove(render_path, ignored);
        std::filesystem::remove(texture_path, ignored);
    }
    return result;
}

absl::Status GpuSnapshotFiles::SaveFiles(const std::filesystem::path& render_path,
                                         const std::filesystem::path& texture_path,
                                         gfxstream::Renderer* renderer) {
    FileStream render_stream(render_path, std::ios::out | std::ios::trunc);
    if (!render_stream.IsValid()) {
        return absl::InternalError(
                absl::StrCat("failed to open '", render_path.string(), "' for save"));
    }
    auto texture_saver = std::make_shared<FileTextureSaver>(texture_path);
    if (!renderer->save(&render_stream, texture_saver)) {
        return absl::InternalError(absl::StrCat("gfxstream Renderer::save() failed writing '",
                                                render_path.string(), "' and '",
                                                texture_path.string(), "'",
                                                StreamErrors(render_stream)));
    }
    if (absl::Status st = texture_saver->Done(); !st.ok()) {
        return st;
    }
    render_stream.Flush();
    if (!render_stream.IsValid() || render_stream.hasErrors()) {
        return absl::DataLossError(absl::StrCat("failed to write render stream '",
                                                render_path.string(), "'",
                                                StreamErrors(render_stream)));
    }
    return absl::OkStatus();
}

absl::StatusOr<gfxstream::ITextureLoaderPtr> GpuSnapshotFiles::Load(
        std::string_view snapshot_name, gfxstream::Renderer* renderer) const {
    if (!IsOnDisk(snapshot_name)) {
        return gfxstream::ITextureLoaderPtr{};
    }
    RETURN_IF_ERROR(ValidateName(snapshot_name));
    const std::filesystem::path dir = Directory(snapshot_name);
    const std::filesystem::path render_path = dir / kRenderStreamFileName;
    const std::filesystem::path texture_path = dir / kTextureFileName;
    FileStream render_stream(render_path, std::ios::in);
    if (!render_stream.IsValid()) {
        return absl::DataLossError(
                absl::StrCat("failed to open '", render_path.string(), "' for load"));
    }
    auto texture_loader = std::make_shared<FileTextureLoader>(texture_path);
    if (absl::Status st = texture_loader->Start(); !st.ok()) {
        return st;
    }
    if (!renderer->load(&render_stream, texture_loader)) {
        return absl::DataLossError(absl::StrCat("gfxstream Renderer::load() failed reading '",
                                                render_path.string(), "' and '",
                                                texture_path.string(), "'",
                                                StreamErrors(render_stream)));
    }
    return gfxstream::ITextureLoaderPtr(std::move(texture_loader));
}

absl::Status GpuSnapshotFiles::ArtifactsPresent(std::string_view snapshot_name) const {
    if (!IsOnDisk(snapshot_name)) {
        return absl::OkStatus();
    }
    RETURN_IF_ERROR(ValidateName(snapshot_name));
    const std::filesystem::path dir = Directory(snapshot_name);
    for (const char* filename : {kRenderStreamFileName, kTextureFileName}) {
        const std::filesystem::path path = dir / filename;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) {
            return absl::DataLossError(absl::StrCat("missing or non-regular snapshot artifact '",
                                                    path.string(), "'"));
        }
    }
    return absl::OkStatus();
}

}  // namespace goldfish::legacy
