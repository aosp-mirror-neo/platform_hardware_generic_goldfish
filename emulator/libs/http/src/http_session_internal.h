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
#pragma once

#include <llhttp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"

#include "goldfish/http/http_headers.h"
#include "goldfish/http/http_request.h"
#include "goldfish/http/http_response.h"

namespace goldfish::http::internal {

inline constexpr size_t kLlhttpMethodTableSize =
        static_cast<size_t>(std::max({HTTP_DELETE, HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT,
                                      HTTP_OPTIONS, HTTP_PATCH})) +
        1;

inline constexpr auto kLlhttpMethodTable = []() {
    std::array<std::optional<HttpMethod>, kLlhttpMethodTableSize> table{};
    table[HTTP_DELETE] = HttpMethod::kDelete;
    table[HTTP_GET] = HttpMethod::kGet;
    table[HTTP_HEAD] = HttpMethod::kHead;
    table[HTTP_POST] = HttpMethod::kPost;
    table[HTTP_PUT] = HttpMethod::kPut;
    table[HTTP_OPTIONS] = HttpMethod::kOptions;
    table[HTTP_PATCH] = HttpMethod::kPatch;
    return table;
}();

// Maps a parsed `llhttp_method_t` enum value directly to an `HttpMethod`
// without string lookup, returning `std::nullopt` for unsupported or
// out-of-range method codes.
constexpr std::optional<HttpMethod> LlhttpMethodToHttpMethod(llhttp_method_t method) noexcept {
    const auto index = static_cast<size_t>(method);
    if (index >= kLlhttpMethodTable.size()) {
        return std::nullopt;
    }
    return kLlhttpMethodTable[index];
}

inline char* AppendWirePiece(char* dst, std::string_view piece) noexcept {
    if (!piece.empty()) {
        std::memcpy(dst, piece.data(), piece.size());
        dst += piece.size();
    }
    return dst;
}

inline char* AppendWireHeaderLine(char* dst, std::string_view name,
                                  std::string_view value) noexcept {
    dst = AppendWirePiece(dst, name);
    dst = AppendWirePiece(dst, protocol::kColonSpace);
    dst = AppendWirePiece(dst, value);
    return AppendWirePiece(dst, protocol::kCrlf);
}

// Formats the HTTP/1.1 status line and response headers for a streaming
// response in a single allocation and reports whether `headers` enables
// `Transfer-Encoding: chunked`.
inline std::string FormatStreamingHeaders(
        const absl::flat_hash_map<std::string, std::string>& headers, int status,
        bool* is_chunked = nullptr) {
    const absl::AlphaNum status_num(status);
    const std::string_view reason = HttpStatusToReason(status);

    bool chunked = false;
    size_t headers_size = 0;
    for (const auto& [name, value] : headers) {
        headers_size +=
                name.size() + protocol::kColonSpace.size() + value.size() + protocol::kCrlf.size();
        if (!chunked && CaseInsensitiveStringEq{}(name, headers::kTransferEncoding) &&
            CaseInsensitiveStringEq{}(value, "chunked")) {
            chunked = true;
        }
    }
    if (is_chunked != nullptr) {
        *is_chunked = chunked;
    }

    const size_t total_size = protocol::kHttp11.size() + protocol::kSpace.size() +
                              status_num.size() + protocol::kSpace.size() + reason.size() +
                              protocol::kCrlf.size() + headers_size + protocol::kCrlf.size();

    std::string response(total_size, '\0');
    char* cursor = response.data();
    cursor = AppendWirePiece(cursor, protocol::kHttp11);
    cursor = AppendWirePiece(cursor, protocol::kSpace);
    cursor = AppendWirePiece(cursor, status_num.Piece());
    cursor = AppendWirePiece(cursor, protocol::kSpace);
    cursor = AppendWirePiece(cursor, reason);
    cursor = AppendWirePiece(cursor, protocol::kCrlf);
    for (const auto& [name, value] : headers) {
        cursor = AppendWireHeaderLine(cursor, name, value);
    }
    AppendWirePiece(cursor, protocol::kCrlf);
    return response;
}

}  // namespace goldfish::http::internal
