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

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "goldfish/async/async_socket.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/http/http_headers.h"
#include "goldfish/http/http_request.h"
#include "goldfish/http/http_response.h"
#include "goldfish/http/http_response_writer.h"
#include "goldfish/http/http_router.h"
#include "goldfish/network/endpoint.h"

namespace goldfish::http {

/**
 * @brief Structured record representing a completed HTTP request/response exchange.
 */
struct AccessLogEntry {
    absl::Time timestamp;                  ///< Start time of the request processing.
    network::Endpoint client_endpoint;     ///< Remote client IP and port.
    HttpMethod method = HttpMethod::kGet;  ///< HTTP method verb.
    std::string path;                      ///< Full URL path requested.
    int status_code = 0;                   ///< HTTP status code returned.
    size_t bytes_sent = 0;                 ///< Response payload bytes emitted.
    absl::Duration duration;               ///< Request processing elapsed duration.
};

/**
 * @brief Callback function type for receiving access log records.
 */
using AccessLogger = std::function<void(const AccessLogEntry& entry)>;

/**
 * @brief Manages HTTP/1.1 stream parsing, Keep-Alive state, and route dispatching
 *        for a single client TCP socket connection.
 *
 * Wraps @llhttp for high-throughput zero-copy parsing. Manages response transmission,
 * Keep-Alive connection reset, and early payload limit enforcement (HTTP 413).
 *
 * Threading Model (Dijkstra Monitor Pattern):
 * - llhttp execution and socket reads run strictly on the EventLoop thread.
 * - Sockets and user callbacks are safely detached outside of internal mutex locks.
 */
class HttpSession : public std::enable_shared_from_this<HttpSession> {
  private:
    struct Passkey {
        explicit Passkey() = default;
    };

  public:
    using OnDetachedCallback = std::function<void(const std::shared_ptr<HttpSession>&)>;

    /**
     * @brief Creates a shared HttpSession instance bound to a client socket and router.
     * @param socket Client async socket connection.
     * @param loop EventLoop managing socket I/O.
     * @param router Route registry for dispatching matched requests.
     * @param max_payload_size Maximum permitted body size in bytes before returning 413.
     * @param not_found_handler Optional custom 404 handler.
     * @param access_logger Optional access log sink for request telemetry.
     * @param client_endpoint Remote endpoint of the connected client.
     */
    static std::shared_ptr<HttpSession> Create(std::shared_ptr<async::AsyncSocket> socket,
                                               async::EventLoop* loop, const HttpRouter* router,
                                               size_t max_payload_size = 64 * 1024 * 1024,
                                               HttpHandler not_found_handler = nullptr,
                                               AccessLogger access_logger = nullptr,
                                               network::Endpoint client_endpoint = {});

    HttpSession(Passkey, std::shared_ptr<async::AsyncSocket> socket, async::EventLoop* loop,
                const HttpRouter* router, size_t max_payload_size, HttpHandler not_found_handler,
                AccessLogger access_logger, network::Endpoint client_endpoint = {});
    ~HttpSession();

    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;

    /**
     * @brief Sets the remote client endpoint address.
     */
    void SetRemoteEndpoint(network::Endpoint endpoint) { remote_endpoint_ = std::move(endpoint); }

    /**
     * @brief Returns the remote client endpoint address.
     */
    const network::Endpoint& RemoteEndpoint() const { return remote_endpoint_; }

    /**
     * @brief Feeds incoming raw bytes from the socket into llhttp.
     * @param data Buffer view received from socket read.
     * @return true on successful parse, false on protocol parse error.
     */
    bool OnDataReceived(std::string_view data);

    /**
     * @brief Detaches the socket when closed or terminating.
     */
    void DetachSocket();

    /**
     * @brief Resets parser state for HTTP/1.1 Keep-Alive requests on the same connection.
     */
    void Reset();

    /**
     * @brief Sets a callback invoked when the session is detached or its connection terminates.
     * @param on_detached Callback accepting a shared pointer to this session.
     */
    void SetOnDetachedCallback(OnDetachedCallback on_detached);

    /**
     * @brief Returns the unique monotonic identifier for this session.
     */
    uint64_t SessionId() const { return session_id_; }

  private:
    class ResponseWriterImpl;
    friend class ResponseWriterImpl;

    // llhttp C-style callbacks
    static int OnMessageBegin(llhttp_t* parser);
    static int OnUrl(llhttp_t* parser, const char* at, size_t length);
    static int OnHeaderField(llhttp_t* parser, const char* at, size_t length);
    static int OnHeaderValue(llhttp_t* parser, const char* at, size_t length);
    static int OnHeaderValueComplete(llhttp_t* parser);
    static int OnHeadersComplete(llhttp_t* parser);
    static int OnBody(llhttp_t* parser, const char* at, size_t length);
    static int OnMessageComplete(llhttp_t* parser);

    void DispatchRequest();
    void SendUnaryResponse(const HttpResponse& resp);
    int AppendHeaderBytes(std::string& target, const char* at, size_t length);
    void CommitCurrentHeader();
    void SendHttpError(int status_code, std::string_view reason);
    void EmitAccessLog(HttpMethod method, std::string_view path, int status_code, size_t bytes_sent,
                       absl::Time start_time);
    void EmitAccessLog(int status_code, size_t bytes_sent) {
        EmitAccessLog(current_request_.Method(), current_request_.Path(), status_code, bytes_sent,
                      request_start_time_);
    }

    void PostOrSend(std::string data);
    void PostClose();
    void SendRawBytes(std::string payload);
    void CloseSocketDirect();
    void ResetDirect();
    void FinishStreaming();
    std::shared_ptr<async::AsyncSocket> GetSocket();
    std::shared_ptr<async::AsyncSocket> TakeSocket();

    template <typename F>
    void PostOrRun(F&& fn) {
        if (loop_ != nullptr && !loop_->IsOnLoopThread()) {
            if (loop_->GetState() == async::LooperStatusEvent::State::kRunning) {
                loop_->Post(std::forward<F>(fn)).IgnoreError();
            }
        } else {
            fn();
        }
    }

    const uint64_t session_id_;
    llhttp_t parser_;
    llhttp_settings_t settings_;

    absl::Mutex socket_mutex_;
    std::shared_ptr<async::AsyncSocket> socket_ ABSL_GUARDED_BY(socket_mutex_);
    async::EventLoop* loop_;
    const HttpRouter* router_;
    const size_t max_payload_size_;
    HttpHandler not_found_handler_;
    const AccessLogger access_logger_;
    network::Endpoint remote_endpoint_;

    absl::Mutex detached_mutex_;
    OnDetachedCallback on_detached_ ABSL_GUARDED_BY(detached_mutex_);

    absl::Mutex close_cb_mutex_;
    bool is_detached_ ABSL_GUARDED_BY(close_cb_mutex_) = false;
    std::function<void()> on_client_closed_ ABSL_GUARDED_BY(close_cb_mutex_);

    enum class HeaderState { kNone, kField, kValue };
    HeaderState header_state_ = HeaderState::kNone;
    std::string current_header_field_;
    std::string current_header_value_;
    size_t current_headers_bytes_ = 0;

    static constexpr size_t kMaxHeaderSize = 64 * 1024;

    HttpRequest current_request_;
    bool payload_too_large_ = false;
    bool error_occurred_ = false;
    bool is_streaming_active_ = false;
    std::string pipelined_buffer_;
    absl::Time request_start_time_;
};

}  // namespace goldfish::http
