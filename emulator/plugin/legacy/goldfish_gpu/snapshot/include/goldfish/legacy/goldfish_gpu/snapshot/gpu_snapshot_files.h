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

#include <filesystem>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "render-utils/Renderer.h"
#include "render-utils/snapshot_operations.h"

// Persists the gfxstream renderer state of a snapshot on disk:
//
//   <snapshot_dir>/<name>/gfxstream_render.bin    Renderer::save() stream
//   <snapshot_dir>/<name>/gfxstream_textures.bin  FileTextureSaver output
//
// When no snapshot directory is configured (or the snapshot has no name) the
// renderer is still driven through save() with an in-memory sink so that its
// internal snapshot bookkeeping runs, but nothing is written and Load() is a
// no-op: such a snapshot cannot restore renderer state, and GoldfishGpu::Load
// refuses a meta blob in that mode.

namespace goldfish::legacy {

class GpuSnapshotFiles {
  public:
    static constexpr char kRenderStreamFileName[] = "gfxstream_render.bin";
    static constexpr char kTextureFileName[] = "gfxstream_textures.bin";

    // `snapshot_dir` may be empty (in-memory mode).
    explicit GpuSnapshotFiles(std::filesystem::path snapshot_dir);

    // True when Save()/Load() write to / read from disk for `snapshot_name`.
    bool IsOnDisk(std::string_view snapshot_name) const;
    std::filesystem::path Directory(std::string_view snapshot_name) const;

    // Creates the snapshot directory and writes both files through
    // `renderer->save()`. INTERNAL when the directory or a file cannot be
    // created or the renderer refuses; DATA_LOSS when a write failed.
    absl::Status Save(std::string_view snapshot_name, gfxstream::Renderer* renderer) const;

    // Validates the texture file first (gfxstream aborts with GFXSTREAM_FATAL
    // when ITextureLoader::start() fails inside Renderer::load(), so a missing
    // or truncated file must be turned into a DATA_LOSS error here), then runs
    // `renderer->load()`. Returns the texture loader, which the caller keeps
    // alive while gfxstream lazily loads textures from it; null in in-memory
    // mode.
    absl::StatusOr<gfxstream::ITextureLoaderPtr> Load(std::string_view snapshot_name,
                                                      gfxstream::Renderer* renderer) const;

    // OK when both artifacts exist as regular files (or in in-memory mode);
    // otherwise DATA_LOSS naming the first missing one.
    absl::Status ArtifactsPresent(std::string_view snapshot_name) const;

    // INVALID_ARGUMENT when `snapshot_name` is not a single plain path
    // component (absolute, contains a separator, or is "."/".."), so a name
    // can never address a file outside `snapshot_dir`.
    static absl::Status ValidateName(std::string_view snapshot_name);

  private:
    // Writes both files; on failure the caller removes whatever was written.
    static absl::Status SaveFiles(const std::filesystem::path& render_path,
                                  const std::filesystem::path& texture_path,
                                  gfxstream::Renderer* renderer);

    std::filesystem::path snapshot_dir_;
};

}  // namespace goldfish::legacy
