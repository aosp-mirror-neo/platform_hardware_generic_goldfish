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

#include <llhttp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/hash/hash.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "benchmark/benchmark.h"

#include "goldfish/http/http_headers.h"
#include "goldfish/http/http_request.h"
#include "goldfish/http/http_response.h"
#include "goldfish/http/http_router.h"
#include "goldfish/http/http_session.h"
#include "http_session_internal.h"

namespace goldfish::http {
namespace {

struct LegacyCaseInsensitiveStringHash {
    using is_transparent = void;

    size_t operator()(std::string_view sv) const noexcept {
        return absl::Hash<Wrapper>{}(Wrapper{sv});
    }

  private:
    struct Wrapper {
        std::string_view sv;
        template <typename H>
        friend H AbslHashValue(H h, Wrapper w) {
            for (char c : w.sv) {
                h = H::combine(std::move(h), static_cast<unsigned char>(absl::ascii_tolower(c)));
            }
            return H::combine(std::move(h), w.sv.size());
        }
    };
};

struct LegacyCaseInsensitiveStringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return absl::EqualsIgnoreCase(a, b);
    }
};

using LegacyHeaderMap =
        absl::flat_hash_map<std::string, std::string, LegacyCaseInsensitiveStringHash,
                            LegacyCaseInsensitiveStringEq>;

constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kSampleHeaders = {{
    {"host", "127.0.0.1:8554"},
    {"content-type", "application/grpc-web+proto"},
    {"content-length", "128"},
    {"connection", "keep-alive"},
    {"origin", "http://localhost:8080"},
    {"authorization", "Bearer eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9"},
    {"x-grpc-web", "1"},
    {"access-control-request-headers", "content-type,x-grpc-web,authorization"},
}};

constexpr std::array<std::string_view, 8> kLookupKeys = {{
    "Host",
    "Content-Type",
    "Content-Length",
    "Connection",
    "Origin",
    "Authorization",
    "X-Grpc-Web",
    "Access-Control-Request-Headers",
}};

constexpr std::array<std::string_view, 4> kCorsLookupKeys = {{
    "Access-Control-Allow-Origin",
    "Access-Control-Allow-Methods",
    "Access-Control-Allow-Headers",
    "Access-Control-Request-Headers",
}};

void BmCaseInsensitiveHeaderLookupBaseline(benchmark::State& state) {
    LegacyHeaderMap headers;
    for (const auto& [k, v] : kSampleHeaders) {
        headers.emplace(k, v);
    }

    size_t idx = 0;
    for (auto _ : state) {
        auto it = headers.find(kLookupKeys[idx & 7]);
        benchmark::DoNotOptimize(it);
        ++idx;
    }
}
BENCHMARK(BmCaseInsensitiveHeaderLookupBaseline);

void BmCaseInsensitiveHeaderLookup(benchmark::State& state) {
    HttpRequest::HeaderMap headers;
    for (const auto& [k, v] : kSampleHeaders) {
        headers.emplace(k, v);
    }

    size_t idx = 0;
    for (auto _ : state) {
        auto it = headers.find(kLookupKeys[idx & 7]);
        benchmark::DoNotOptimize(it);
        ++idx;
    }
}
BENCHMARK(BmCaseInsensitiveHeaderLookup);

void BmCaseInsensitiveCorsHeaderLookupBaseline(benchmark::State& state) {
    LegacyHeaderMap headers = {
        {"access-control-allow-origin", "*"},
        {"access-control-allow-methods", "GET, POST, OPTIONS"},
        {"access-control-allow-headers", "content-type,x-grpc-web,authorization"},
        {"access-control-request-headers", "content-type,x-grpc-web"},
    };

    size_t idx = 0;
    for (auto _ : state) {
        auto it = headers.find(kCorsLookupKeys[idx & 3]);
        benchmark::DoNotOptimize(it);
        ++idx;
    }
}
BENCHMARK(BmCaseInsensitiveCorsHeaderLookupBaseline);

void BmCaseInsensitiveCorsHeaderLookup(benchmark::State& state) {
    HttpRequest::HeaderMap headers = {
        {"access-control-allow-origin", "*"},
        {"access-control-allow-methods", "GET, POST, OPTIONS"},
        {"access-control-allow-headers", "content-type,x-grpc-web,authorization"},
        {"access-control-request-headers", "content-type,x-grpc-web"},
    };

    size_t idx = 0;
    for (auto _ : state) {
        auto it = headers.find(kCorsLookupKeys[idx & 3]);
        benchmark::DoNotOptimize(it);
        ++idx;
    }
}
BENCHMARK(BmCaseInsensitiveCorsHeaderLookup);

class LegacyHttpRouter {
  public:
    void AddRoute(HttpMethod method, std::string path, HttpHandler handler) {
        RouteHandler rh;
        rh.kind = RouteHandler::Kind::kUnary;
        rh.unary_handler = std::move(handler);
        AddRouteHandler(method, std::move(path), std::move(rh));
    }

    void AddStreamingRoute(HttpMethod method, std::string path, AsyncHttpHandler handler) {
        RouteHandler rh;
        rh.kind = RouteHandler::Kind::kStreaming;
        rh.streaming_handler = std::move(handler);
        AddRouteHandler(method, std::move(path), std::move(rh));
    }

    std::optional<RouteHandler> Match(HttpMethod method, std::string_view path) const {
        auto it = exact_routes_.find(path);
        if (it != exact_routes_.end()) {
            auto method_it = it->second.find(method);
            if (method_it != it->second.end()) {
                return method_it->second;
            }
            auto any_it = it->second.find(HttpMethod::kAny);
            if (any_it != it->second.end()) {
                return any_it->second;
            }
        }
        for (const auto& w : wildcard_routes_) {
            if ((w.method == method || w.method == HttpMethod::kAny) &&
                absl::StartsWith(path, w.prefix)) {
                return w.handler;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> GetAllowedMethods(std::string_view path) const {
        absl::flat_hash_set<std::string> allowed_set;
        auto exact_it = methods_by_exact_path_.find(path);
        if (exact_it != methods_by_exact_path_.end()) {
            for (HttpMethod m : exact_it->second) {
                allowed_set.insert(std::string(HttpMethodToString(m)));
            }
        }
        for (const auto& w : wildcard_routes_) {
            if (absl::StartsWith(path, w.prefix)) {
                allowed_set.insert(std::string(HttpMethodToString(w.method)));
            }
        }
        std::vector<std::string> allowed(allowed_set.begin(), allowed_set.end());
        std::sort(allowed.begin(), allowed.end());
        return allowed;
    }

  private:
    struct WildcardRoute {
        HttpMethod method;
        std::string prefix;
        RouteHandler handler;
    };

    void AddRouteHandler(HttpMethod method, std::string path, RouteHandler rh) {
        size_t star = path.find('*');
        if (star != std::string::npos) {
            WildcardRoute wr{method, path.substr(0, star), std::move(rh)};
            auto it = std::lower_bound(wildcard_routes_.begin(), wildcard_routes_.end(), wr,
                                       [](const WildcardRoute& a, const WildcardRoute& b) {
                                           return a.prefix.size() > b.prefix.size();
                                       });
            wildcard_routes_.insert(it, std::move(wr));
            return;
        }
        methods_by_exact_path_[path].insert(method);
        exact_routes_[std::move(path)].insert_or_assign(method, std::move(rh));
    }

    TransparentStringMap<absl::flat_hash_map<HttpMethod, RouteHandler>> exact_routes_;
    std::vector<WildcardRoute> wildcard_routes_;
    TransparentStringMap<absl::flat_hash_set<HttpMethod>> methods_by_exact_path_;
};

constexpr std::array<std::string_view, 4> kExactRoutePaths = {{
    "/api/v1/status",
    "/api/v1/devices",
    "/api/v1/metrics",
    "/healthz",
}};

template <typename RouterT>
void PopulateSampleRouter(RouterT& router) {
    auto shared_channel = std::make_shared<int>(42);
    auto shared_tracker = std::make_shared<int>(7);
    std::string allowed_origin = "http://localhost:8080,http://127.0.0.1:8080";

    for (std::string_view route : kExactRoutePaths) {
        router.AddRoute(HttpMethod::kGet, std::string(route),
                        [shared_channel, shared_tracker, allowed_origin](const HttpRequest&) {
                            return HttpResponse::Empty(200);
                        });
        router.AddRoute(HttpMethod::kPost, std::string(route),
                        [shared_channel, shared_tracker, allowed_origin](const HttpRequest&) {
                            return HttpResponse::Empty(200);
                        });
    }
    router.AddRoute(HttpMethod::kOptions, "/*",
                    [allowed_origin](const HttpRequest&) { return HttpResponse::Empty(204); });
    router.AddStreamingRoute(HttpMethod::kPost, "/*",
                             [shared_channel, allowed_origin, shared_tracker](
                                     const HttpRequest&, std::shared_ptr<HttpResponseWriter>) {});
}

void BmHttpRouterExactMatchBaseline(benchmark::State& state) {
    LegacyHttpRouter router;
    PopulateSampleRouter(router);

    size_t idx = 0;
    for (auto _ : state) {
        auto match = router.Match(HttpMethod::kGet, kExactRoutePaths[idx & 3]);
        benchmark::DoNotOptimize(match);
        ++idx;
    }
}
BENCHMARK(BmHttpRouterExactMatchBaseline);

void BmHttpRouterExactMatchHandler(benchmark::State& state) {
    HttpRouter router;
    PopulateSampleRouter(router);

    size_t idx = 0;
    for (auto _ : state) {
        const RouteHandler* match =
                router.MatchHandler(HttpMethod::kGet, kExactRoutePaths[idx & 3]);
        benchmark::DoNotOptimize(match);
        ++idx;
    }
}
BENCHMARK(BmHttpRouterExactMatchHandler);

void BmHttpRouterWildcardStreamMatchBaseline(benchmark::State& state) {
    LegacyHttpRouter router;
    PopulateSampleRouter(router);
    constexpr std::string_view kGrpcPath =
            "/android.emulation.control.EmulatorController/streamScreenshot";

    for (auto _ : state) {
        auto match = router.Match(HttpMethod::kPost, kGrpcPath);
        benchmark::DoNotOptimize(match);
    }
}
BENCHMARK(BmHttpRouterWildcardStreamMatchBaseline);

void BmHttpRouterWildcardStreamMatchHandler(benchmark::State& state) {
    HttpRouter router;
    PopulateSampleRouter(router);
    constexpr std::string_view kGrpcPath =
            "/android.emulation.control.EmulatorController/streamScreenshot";

    for (auto _ : state) {
        const RouteHandler* match = router.MatchHandler(HttpMethod::kPost, kGrpcPath);
        benchmark::DoNotOptimize(match);
    }
}
BENCHMARK(BmHttpRouterWildcardStreamMatchHandler);

void BmHttpRouterGetAllowedMethodsBaseline(benchmark::State& state) {
    LegacyHttpRouter router;
    PopulateSampleRouter(router);

    size_t idx = 0;
    for (auto _ : state) {
        auto allowed = router.GetAllowedMethods(kExactRoutePaths[idx & 3]);
        benchmark::DoNotOptimize(allowed);
        ++idx;
    }
}
BENCHMARK(BmHttpRouterGetAllowedMethodsBaseline);

void BmHttpRouterGetAllowedMethods(benchmark::State& state) {
    HttpRouter router;
    PopulateSampleRouter(router);

    size_t idx = 0;
    for (auto _ : state) {
        auto allowed = router.GetAllowedMethods(kExactRoutePaths[idx & 3]);
        benchmark::DoNotOptimize(allowed);
        ++idx;
    }
}
BENCHMARK(BmHttpRouterGetAllowedMethods);

ABSL_ATTRIBUTE_NOINLINE std::optional<HttpMethod> LegacyStringToHttpMethod(
        std::string_view method) {
    if (absl::EqualsIgnoreCase(method, methods::kGet)) {
        return HttpMethod::kGet;
    }
    if (absl::EqualsIgnoreCase(method, methods::kPost)) {
        return HttpMethod::kPost;
    }
    if (absl::EqualsIgnoreCase(method, methods::kPut)) {
        return HttpMethod::kPut;
    }
    if (absl::EqualsIgnoreCase(method, methods::kDelete)) {
        return HttpMethod::kDelete;
    }
    if (absl::EqualsIgnoreCase(method, methods::kOptions)) {
        return HttpMethod::kOptions;
    }
    if (absl::EqualsIgnoreCase(method, methods::kHead)) {
        return HttpMethod::kHead;
    }
    if (absl::EqualsIgnoreCase(method, methods::kPatch)) {
        return HttpMethod::kPatch;
    }
    if (method == methods::kAny) {
        return HttpMethod::kAny;
    }
    return std::nullopt;
}

constexpr std::array<std::string_view, 8> kMethodStringInputs = {{
    "POST",
    "OPTIONS",
    "GET",
    "DELETE",
    "PATCH",
    "HEAD",
    "post",
    "CONNECT",
}};

constexpr std::array<llhttp_method_t, 8> kLlhttpMethodInputs = {{
    HTTP_POST,
    HTTP_OPTIONS,
    HTTP_GET,
    HTTP_POST,
    HTTP_OPTIONS,
    HTTP_DELETE,
    HTTP_HEAD,
    HTTP_PATCH,
}};

void BmStringToHttpMethodBaseline(benchmark::State& state) {
    size_t idx = 0;
    for (auto _ : state) {
        std::string_view input = kMethodStringInputs[idx & 7];
        benchmark::DoNotOptimize(input);
        auto parsed = LegacyStringToHttpMethod(input);
        benchmark::DoNotOptimize(parsed);
        ++idx;
    }
}
BENCHMARK(BmStringToHttpMethodBaseline);

void BmStringToHttpMethod(benchmark::State& state) {
    size_t idx = 0;
    for (auto _ : state) {
        std::string_view input = kMethodStringInputs[idx & 7];
        benchmark::DoNotOptimize(input);
        auto parsed = StringToHttpMethod(input);
        benchmark::DoNotOptimize(parsed);
        ++idx;
    }
}
BENCHMARK(BmStringToHttpMethod);

constexpr std::optional<HttpMethod> SwitchLlhttpMethodToHttpMethod(
        llhttp_method_t method) noexcept {
    switch (method) {
    case HTTP_GET:
        return HttpMethod::kGet;
    case HTTP_POST:
        return HttpMethod::kPost;
    case HTTP_PUT:
        return HttpMethod::kPut;
    case HTTP_DELETE:
        return HttpMethod::kDelete;
    case HTTP_OPTIONS:
        return HttpMethod::kOptions;
    case HTTP_HEAD:
        return HttpMethod::kHead;
    case HTTP_PATCH:
        return HttpMethod::kPatch;
    default:
        return std::nullopt;
    }
}

void BmLlhttpMethodParseBaseline(benchmark::State& state) {
    size_t idx = 0;
    for (auto _ : state) {
        llhttp_method_t input = kLlhttpMethodInputs[idx & 7];
        benchmark::DoNotOptimize(input);
        HttpMethod method =
                LegacyStringToHttpMethod(llhttp_method_name(input)).value_or(HttpMethod::kGet);
        benchmark::DoNotOptimize(method);
        ++idx;
    }
}
BENCHMARK(BmLlhttpMethodParseBaseline);

void BmLlhttpMethodParseSwitch(benchmark::State& state) {
    size_t idx = 0;
    for (auto _ : state) {
        llhttp_method_t input = kLlhttpMethodInputs[idx & 7];
        benchmark::DoNotOptimize(input);
        HttpMethod method = SwitchLlhttpMethodToHttpMethod(input).value_or(HttpMethod::kGet);
        benchmark::DoNotOptimize(method);
        ++idx;
    }
}
BENCHMARK(BmLlhttpMethodParseSwitch);

void BmLlhttpMethodParse(benchmark::State& state) {
    size_t idx = 0;
    for (auto _ : state) {
        llhttp_method_t input = kLlhttpMethodInputs[idx & 7];
        benchmark::DoNotOptimize(input);
        HttpMethod method = internal::LlhttpMethodToHttpMethod(input).value_or(HttpMethod::kGet);
        benchmark::DoNotOptimize(method);
        ++idx;
    }
}
BENCHMARK(BmLlhttpMethodParse);

ABSL_ATTRIBUTE_NOINLINE std::string LegacyFormatWireResponse(const HttpResponse& resp,
                                                             bool keep_alive,
                                                             bool is_head_request = false) {
    std::string response;
    const std::string& body = resp.Body();
    const int status = resp.StatusCode();
    response.reserve(body.size() + 256);
    absl::StrAppend(&response, protocol::kHttp11, protocol::kSpace, status, protocol::kSpace,
                    HttpStatusToReason(status), protocol::kCrlf);

    bool has_content_length = false;
    bool has_connection = false;

    for (const auto& [name, value] : resp.Headers()) {
        absl::StrAppend(&response, name, protocol::kColonSpace, value, protocol::kCrlf);
        if (absl::EqualsIgnoreCase(name, headers::kContentLength)) {
            has_content_length = true;
        }
        if (absl::EqualsIgnoreCase(name, headers::kConnection)) {
            has_connection = true;
        }
    }

    const bool forbids_content_length = (status >= 100 && status < 200) || status == 204;
    if (!has_content_length && !forbids_content_length) {
        absl::StrAppend(&response, headers::kContentLength, protocol::kColonSpace, body.size(),
                        protocol::kCrlf);
    }
    if (!has_connection) {
        absl::StrAppend(&response, headers::kConnection, protocol::kColonSpace,
                        keep_alive ? protocol::kKeepAlive : protocol::kClose, protocol::kCrlf);
    }

    absl::StrAppend(&response, protocol::kCrlf);
    if (!is_head_request && status != 204 && status != 304 && (status < 100 || status >= 200)) {
        absl::StrAppend(&response, body);
    }
    return response;
}

HttpResponse BuildJsonResponse() {
    return HttpResponse::String("{\"status\":\"ok\",\"emulator\":\"goldfish\",\"port\":8554}",
                                HttpStatus::kOk, mime::kApplicationJson);
}

HttpResponse BuildCorsJsonResponse() {
    return BuildJsonResponse()
            .WithHeader(headers::kAccessControlAllowOrigin, "http://localhost:8080")
            .WithHeader(headers::kAccessControlAllowMethods,
                        "GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH")
            .WithHeader(headers::kAccessControlAllowHeaders,
                        "content-type,x-grpc-web,x-user-agent,authorization")
            .WithHeader(headers::kAccessControlExposeHeaders,
                        "grpc-status,grpc-message,grpc-status-details-bin")
            .WithHeader(headers::kCacheControl, "no-store, no-cache, must-revalidate")
            .WithHeader(headers::kXGrpcWeb, "1");
}

void BmFormatWireResponseJsonBaseline(benchmark::State& state) {
    const HttpResponse resp = BuildJsonResponse();
    for (auto _ : state) {
        std::string wire = LegacyFormatWireResponse(resp, /*keep_alive=*/true);
        benchmark::DoNotOptimize(wire);
    }
}
BENCHMARK(BmFormatWireResponseJsonBaseline);

void BmFormatWireResponseJson(benchmark::State& state) {
    const HttpResponse resp = BuildJsonResponse();
    for (auto _ : state) {
        std::string wire = resp.FormatWireResponse(/*keep_alive=*/true);
        benchmark::DoNotOptimize(wire);
    }
}
BENCHMARK(BmFormatWireResponseJson);

void BmFormatWireResponseCorsHeadersBaseline(benchmark::State& state) {
    const HttpResponse resp = BuildCorsJsonResponse();
    for (auto _ : state) {
        std::string wire = LegacyFormatWireResponse(resp, /*keep_alive=*/true);
        benchmark::DoNotOptimize(wire);
    }
}
BENCHMARK(BmFormatWireResponseCorsHeadersBaseline);

void BmFormatWireResponseCorsHeaders(benchmark::State& state) {
    const HttpResponse resp = BuildCorsJsonResponse();
    for (auto _ : state) {
        std::string wire = resp.FormatWireResponse(/*keep_alive=*/true);
        benchmark::DoNotOptimize(wire);
    }
}
BENCHMARK(BmFormatWireResponseCorsHeaders);

void BmFormatWireResponseHeadLargeBodyBaseline(benchmark::State& state) {
    const HttpResponse resp = HttpResponse::String(std::string(65536, 'x'), HttpStatus::kOk,
                                                   mime::kApplicationOctetStream);
    for (auto _ : state) {
        std::string wire =
                LegacyFormatWireResponse(resp, /*keep_alive=*/true, /*is_head_request=*/true);
        benchmark::DoNotOptimize(wire);
    }
}
BENCHMARK(BmFormatWireResponseHeadLargeBodyBaseline);

void BmFormatWireResponseHeadLargeBody(benchmark::State& state) {
    const HttpResponse resp = HttpResponse::String(std::string(65536, 'x'), HttpStatus::kOk,
                                                   mime::kApplicationOctetStream);
    for (auto _ : state) {
        std::string wire = resp.FormatWireResponse(/*keep_alive=*/true, /*is_head_request=*/true);
        benchmark::DoNotOptimize(wire);
    }
}
BENCHMARK(BmFormatWireResponseHeadLargeBody);

ABSL_ATTRIBUTE_NOINLINE std::string LegacyFormatStreamingHeaders(
        const absl::flat_hash_map<std::string, std::string>& headers, int status,
        bool* is_chunked = nullptr) {
    bool chunked = false;
    for (const auto& [k, v] : headers) {
        if (absl::EqualsIgnoreCase(k, headers::kTransferEncoding) &&
            absl::EqualsIgnoreCase(v, "chunked")) {
            chunked = true;
        }
    }
    if (is_chunked != nullptr) {
        *is_chunked = chunked;
    }
    std::string response;
    absl::StrAppend(&response, protocol::kHttp11, protocol::kSpace, status, protocol::kSpace,
                    HttpStatusToReason(status), protocol::kCrlf);
    for (const auto& [k, v] : headers) {
        absl::StrAppend(&response, k, protocol::kColonSpace, v, protocol::kCrlf);
    }
    absl::StrAppend(&response, protocol::kCrlf);
    return response;
}

absl::flat_hash_map<std::string, std::string> BuildChunkedStreamHeaders() {
    return {
        {std::string(headers::kContentType), std::string(mime::kApplicationOctetStream)},
        {std::string(headers::kTransferEncoding), "chunked"},
    };
}

absl::flat_hash_map<std::string, std::string> BuildGrpcWebStreamHeaders() {
    return {
        {std::string(headers::kContentType), std::string(mime::kApplicationGrpcWebProto)},
        {std::string(headers::kAccessControlExposeHeaders),
         "grpc-status, grpc-message, grpc-status-details-bin"},
        {std::string(headers::kCacheControl), "no-cache, no-store, max-age=0, must-revalidate"},
        {std::string(headers::kTransferEncoding), "chunked"},
        {std::string(headers::kAccessControlAllowOrigin), "http://localhost:8080"},
        {std::string(headers::kAccessControlAllowCredentials), std::string(protocol::kTrue)},
        {std::string(headers::kVary), "Origin"},
    };
}

void BmFormatStreamingHeadersChunkedBaseline(benchmark::State& state) {
    const auto headers = BuildChunkedStreamHeaders();
    for (auto _ : state) {
        bool is_chunked = false;
        std::string wire = LegacyFormatStreamingHeaders(headers, 200, &is_chunked);
        benchmark::DoNotOptimize(wire);
        benchmark::DoNotOptimize(is_chunked);
    }
}
BENCHMARK(BmFormatStreamingHeadersChunkedBaseline);

void BmFormatStreamingHeadersChunked(benchmark::State& state) {
    const auto headers = BuildChunkedStreamHeaders();
    for (auto _ : state) {
        bool is_chunked = false;
        std::string wire = internal::FormatStreamingHeaders(headers, 200, &is_chunked);
        benchmark::DoNotOptimize(wire);
        benchmark::DoNotOptimize(is_chunked);
    }
}
BENCHMARK(BmFormatStreamingHeadersChunked);

void BmFormatStreamingHeadersGrpcWebCorsBaseline(benchmark::State& state) {
    const auto headers = BuildGrpcWebStreamHeaders();
    for (auto _ : state) {
        bool is_chunked = false;
        std::string wire = LegacyFormatStreamingHeaders(headers, 200, &is_chunked);
        benchmark::DoNotOptimize(wire);
        benchmark::DoNotOptimize(is_chunked);
    }
}
BENCHMARK(BmFormatStreamingHeadersGrpcWebCorsBaseline);

void BmFormatStreamingHeadersGrpcWebCors(benchmark::State& state) {
    const auto headers = BuildGrpcWebStreamHeaders();
    for (auto _ : state) {
        bool is_chunked = false;
        std::string wire = internal::FormatStreamingHeaders(headers, 200, &is_chunked);
        benchmark::DoNotOptimize(wire);
        benchmark::DoNotOptimize(is_chunked);
    }
}
BENCHMARK(BmFormatStreamingHeadersGrpcWebCors);

// Baseline reproduction of the previous HttpSession::Reset() implementation:
// unconditionally constructed a shared_from_this() closure for PostOrRun(),
// called absl::flat_hash_map::clear() even when headers_ was already empty
// (which walks control bytes when capacity > 1), and read absl::Now() even
// when no AccessLogger was configured.
struct LegacyHttpSessionResetState
        : public std::enable_shared_from_this<LegacyHttpSessionResetState> {
    absl::Mutex close_cb_mutex;
    std::function<void()> on_client_closed;
    std::string request_path;
    HttpRequest::HeaderMap request_headers;
    std::string request_body;
    bool request_keep_alive = true;
    std::string current_header_field;
    std::string current_header_value;
    size_t current_headers_bytes = 0;
    int header_state = 0;
    bool payload_too_large = false;
    bool error_occurred = false;
    AccessLogger access_logger;
    absl::Time request_start_time = absl::InfinitePast();

    LegacyHttpSessionResetState() {
        // Simulate a Keep-Alive session that has already parsed a typical
        // request so request_headers has an allocated backing table, matching
        // BmHttpSessionReset's steady-state Keep-Alive session.
        request_headers.emplace("host", "127.0.0.1:8554");
        request_headers.emplace("user-agent", "grpc-web-javascript/0.1");
        request_headers.emplace("accept", "application/grpc-web+proto");
        request_headers.emplace("x-grpc-web", "1");
        request_headers.clear();
    }

    template <typename F>
    static void PostOrRunLoop(bool on_loop_thread, F&& fn) {
        if (!on_loop_thread) {
            return;
        }
        fn();
    }

    ABSL_ATTRIBUTE_NOINLINE void ResetBaseline() {
        PostOrRunLoop(/*on_loop_thread=*/true, [self = shared_from_this()]() {
            {
                const absl::MutexLock lock(&self->close_cb_mutex);
                self->on_client_closed = nullptr;
            }
            self->request_path.clear();
            self->request_headers.clear();
            self->request_body.clear();
            self->request_keep_alive = true;
            self->current_header_field.clear();
            self->current_header_value.clear();
            self->current_headers_bytes = 0;
            self->header_state = 0;
            self->payload_too_large = false;
            self->error_occurred = false;
            self->request_start_time = absl::Now();
        });
    }
};

void BmHttpSessionResetBaseline(benchmark::State& state) {
    auto session_state = std::make_shared<LegacyHttpSessionResetState>();
    for (auto _ : state) {
        session_state->ResetBaseline();
        benchmark::DoNotOptimize(session_state.get());
    }
}
BENCHMARK(BmHttpSessionResetBaseline);

void BmHttpSessionReset(benchmark::State& state) {
    HttpRouter router;
    router.AddRoute(HttpMethod::kGet, "/healthz", [](const HttpRequest&) {
        return HttpResponse::String("ok", 200, "text/plain");
    });
    auto session = HttpSession::Create(/*socket=*/nullptr, /*loop=*/nullptr, &router);
    // Warm up the session with one Keep-Alive request so current_request_.headers_
    // has an allocated backing table, matching a steady-state Keep-Alive connection.
    constexpr std::string_view kWarmupRequest =
            "GET /healthz HTTP/1.1\r\n"
            "Host: 127.0.0.1:8554\r\n"
            "User-Agent: grpc-web-javascript/0.1\r\n"
            "Accept: application/grpc-web+proto\r\n"
            "X-Grpc-Web: 1\r\n\r\n";
    session->OnDataReceived(kWarmupRequest);

    for (auto _ : state) {
        session->Reset();
        benchmark::DoNotOptimize(session.get());
    }
}
BENCHMARK(BmHttpSessionReset);

}  // namespace
}  // namespace goldfish::http

BENCHMARK_MAIN();
