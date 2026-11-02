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
#include "goldfish/http/http_session.h"

#include <atomic>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "http_session_internal.h"

namespace goldfish::http {

namespace {

std::atomic<uint64_t> s_session_seq{1};

}  // namespace

class HttpSession::ResponseWriterImpl : public HttpResponseWriter {
  public:
    ResponseWriterImpl(std::shared_ptr<HttpSession> session, HttpMethod method, std::string path,
                       absl::Time start_time, bool keep_alive)
            : session_(std::move(session))
            , method_(method)
            , path_(std::move(path))
            , start_time_(start_time)
            , keep_alive_(keep_alive) {}

    void SendHeaders(const absl::flat_hash_map<std::string, std::string>& headers,
                     int status) override {
        bool expected = false;
        if (!headers_sent_.compare_exchange_strong(expected, true)) {
            return;
        }
        if (finished_.load()) {
            return;
        }
        status_ = status;
        bool is_chunked = false;
        std::string response = internal::FormatStreamingHeaders(headers, status, &is_chunked);
        if (is_chunked) {
            is_chunked_ = true;
        }
        if (auto session = GetSession()) {
            session->PostOrSend(std::move(response));
        }
    }

    void SendChunk(std::string_view chunk) override {
        if (finished_.load() || chunk.empty()) {
            return;
        }
        bytes_sent_ += chunk.size();
        if (auto session = GetSession()) {
            if (is_chunked_) {
                session->PostOrSend(absl::StrCat(absl::Hex(chunk.size()), protocol::kCrlf, chunk,
                                                 protocol::kCrlf));
            } else {
                session->PostOrSend(std::string(chunk));
            }
        }
    }

    void Finish() override {
        if (finished_.exchange(true)) {
            return;
        }
        if (auto session = TakeSession()) {
            {
                const absl::MutexLock lock(&session->close_cb_mutex_);
                session->on_client_closed_ = nullptr;
            }
            session->EmitAccessLog(method_, path_, status_, bytes_sent_, start_time_);
            bool is_chunked = is_chunked_;
            bool keep_alive = keep_alive_;
            auto* raw_session = session.get();
            raw_session->PostOrRun([session = std::move(session), is_chunked, keep_alive]() {
                if (is_chunked) {
                    session->SendRawBytes(absl::StrCat("0", protocol::kDoubleCrlf));
                }
                if (is_chunked && keep_alive) {
                    session->FinishStreaming();
                } else {
                    session->CloseSocketDirect();
                    session->DetachSocket();
                }
            });
        }
    }

    void SetOnCloseCallback(std::function<void()> on_close) override {
        if (auto session = GetSession()) {
            bool call_immediately = false;
            {
                const absl::MutexLock lock(&session->close_cb_mutex_);
                if (session->is_detached_) {
                    call_immediately = true;
                } else {
                    session->on_client_closed_ = std::move(on_close);
                }
            }
            if (call_immediately && on_close) {
                on_close();
            }
        } else if (on_close) {
            on_close();
        }
    }

  private:
    std::shared_ptr<HttpSession> GetSession() {
        const absl::MutexLock lock(&mutex_);
        return session_;
    }

    std::shared_ptr<HttpSession> TakeSession() {
        const absl::MutexLock lock(&mutex_);
        return std::move(session_);
    }

    absl::Mutex mutex_;
    std::shared_ptr<HttpSession> session_ ABSL_GUARDED_BY(mutex_);
    HttpMethod method_ = HttpMethod::kGet;
    std::string path_;
    absl::Time start_time_;
    std::atomic<int> status_{200};
    std::atomic<size_t> bytes_sent_{0};
    std::atomic<bool> headers_sent_{false};
    std::atomic<bool> is_chunked_{false};
    std::atomic<bool> finished_{false};
    bool keep_alive_ = true;
};

std::shared_ptr<HttpSession> HttpSession::Create(std::shared_ptr<async::AsyncSocket> socket,
                                                 async::EventLoop* loop, const HttpRouter* router,
                                                 size_t max_payload_size,
                                                 HttpHandler not_found_handler,
                                                 AccessLogger access_logger,
                                                 network::Endpoint client_endpoint) {
    return std::make_shared<HttpSession>(Passkey{}, std::move(socket), loop, router,
                                         max_payload_size, std::move(not_found_handler),
                                         std::move(access_logger), std::move(client_endpoint));
}

HttpSession::HttpSession(Passkey /*unused*/, std::shared_ptr<async::AsyncSocket> socket,
                         async::EventLoop* loop, const HttpRouter* router, size_t max_payload_size,
                         HttpHandler not_found_handler, AccessLogger access_logger,
                         network::Endpoint client_endpoint)
        : session_id_(s_session_seq.fetch_add(1, std::memory_order_relaxed))
        , socket_(std::move(socket))
        , loop_(loop)
        , router_(router)
        , max_payload_size_(max_payload_size)
        , not_found_handler_(std::move(not_found_handler))
        , access_logger_(std::move(access_logger))
        , remote_endpoint_(std::move(client_endpoint)) {
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = &HttpSession::OnMessageBegin;
    settings_.on_url = &HttpSession::OnUrl;
    settings_.on_header_field = &HttpSession::OnHeaderField;
    settings_.on_header_value = &HttpSession::OnHeaderValue;
    settings_.on_header_value_complete = &HttpSession::OnHeaderValueComplete;
    settings_.on_headers_complete = &HttpSession::OnHeadersComplete;
    settings_.on_body = &HttpSession::OnBody;
    settings_.on_message_complete = &HttpSession::OnMessageComplete;

    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

HttpSession::~HttpSession() {
    DetachSocket();
}

std::shared_ptr<async::AsyncSocket> HttpSession::GetSocket() {
    const absl::MutexLock socket_lock(&socket_mutex_);
    return socket_;
}

std::shared_ptr<async::AsyncSocket> HttpSession::TakeSocket() {
    const absl::MutexLock socket_lock(&socket_mutex_);
    return std::move(socket_);
}

void HttpSession::SendRawBytes(std::string payload) {
    if (auto sock = GetSocket()) {
        auto status = sock->Send(payload.data(), payload.size(),
                                 [self = shared_from_this()](absl::Status err) {
                                     if (!err.ok()) {
                                         self->DetachSocket();
                                     }
                                 });
        if (!status.ok()) {
            DetachSocket();
        }
    }
}

void HttpSession::CloseSocketDirect() {
    if (auto sock = TakeSocket()) {
        sock->Close();
    }
}

void HttpSession::DetachSocket() {
    if (auto sock = TakeSocket()) {
        PostOrRun([sock]() { sock->Close(); });
    }

    std::function<void()> on_close;
    {
        const absl::MutexLock lock(&close_cb_mutex_);
        is_detached_ = true;
        on_close = std::move(on_client_closed_);
    }
    if (on_close) {
        on_close();
    }

    OnDetachedCallback detached_cb;
    {
        const absl::MutexLock lock(&detached_mutex_);
        detached_cb = std::move(on_detached_);
    }
    if (detached_cb) {
        if (auto self = weak_from_this().lock()) {
            detached_cb(self);
        }
    }
}

void HttpSession::SetOnDetachedCallback(OnDetachedCallback on_detached) {
    const absl::MutexLock lock(&detached_mutex_);
    on_detached_ = std::move(on_detached);
}

void HttpSession::Reset() {
    PostOrRun([self = shared_from_this()]() {
        {
            const absl::MutexLock lock(&self->close_cb_mutex_);
            self->on_client_closed_ = nullptr;
        }
        self->current_request_.path_.clear();
        self->current_request_.headers_.clear();
        self->current_request_.body_.clear();
        self->current_request_.keep_alive_ = true;
        self->current_header_field_.clear();
        self->current_header_value_.clear();
        self->current_headers_bytes_ = 0;
        self->header_state_ = HeaderState::kNone;
        self->payload_too_large_ = false;
        self->error_occurred_ = false;
        self->request_start_time_ = absl::Now();
    });
}

void HttpSession::FinishStreaming() {
    is_streaming_active_ = false;
    llhttp_resume(&parser_);
    Reset();
    if (!pipelined_buffer_.empty()) {
        std::string next_data = std::move(pipelined_buffer_);
        pipelined_buffer_.clear();
        OnDataReceived(next_data);
    }
}

void HttpSession::CommitCurrentHeader() {
    if (current_header_field_.empty()) {
        return;
    }
    auto it = current_request_.headers_.find(current_header_field_);
    if (it != current_request_.headers_.end()) {
        it->second.append(", ").append(current_header_value_);
    } else {
        current_request_.headers_.emplace(std::move(current_header_field_),
                                          std::move(current_header_value_));
    }
    current_header_field_.clear();
    current_header_value_.clear();
    header_state_ = HeaderState::kNone;
}

bool HttpSession::OnDataReceived(std::string_view data) {
    if (error_occurred_) {
        return false;
    }
    if (is_streaming_active_) {
        if (pipelined_buffer_.size() + data.size() > max_payload_size_) {
            payload_too_large_ = true;
            error_occurred_ = true;
            SendHttpError(static_cast<int>(HttpStatus::kPayloadTooLarge),
                          HttpStatusToReason(HttpStatus::kPayloadTooLarge));
            return false;
        }
        pipelined_buffer_.append(data);
        return true;
    }

    enum llhttp_errno err = llhttp_execute(&parser_, data.data(), data.size());
    if (err == HPE_PAUSED) {
        const char* error_pos = llhttp_get_error_pos(&parser_);
        size_t consumed = (error_pos != nullptr) ? (error_pos - data.data()) : 0;
        if (consumed < data.size()) {
            pipelined_buffer_.append(data.substr(consumed));
        }
        return true;
    }
    if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
        if (!error_occurred_) {
            error_occurred_ = true;
            if (payload_too_large_) {
                SendHttpError(static_cast<int>(HttpStatus::kPayloadTooLarge),
                              HttpStatusToReason(HttpStatus::kPayloadTooLarge));
            } else {
                const char* reason = llhttp_get_error_reason(&parser_);
                SendHttpError(static_cast<int>(HttpStatus::kBadRequest),
                              reason ? reason : HttpStatusToReason(HttpStatus::kBadRequest));
            }
        }
        return false;
    }
    return true;
}

void HttpSession::PostOrSend(std::string data) {
    PostOrRun([self = shared_from_this(), payload = std::move(data)]() {
        self->SendRawBytes(std::move(payload));
    });
}

void HttpSession::PostClose() {
    PostOrRun([self = shared_from_this()]() { self->CloseSocketDirect(); });
}

void HttpSession::EmitAccessLog(HttpMethod method, std::string_view path, int status_code,
                                size_t bytes_sent, absl::Time start_time) {
    if (access_logger_) {
        absl::Duration duration = (start_time == absl::InfinitePast()) ? absl::ZeroDuration()
                                                                       : (absl::Now() - start_time);
        AccessLogEntry entry{
            .timestamp = start_time,
            .client_endpoint = remote_endpoint_,
            .method = method,
            .path = std::string(path),
            .status_code = status_code,
            .bytes_sent = bytes_sent,
            .duration = duration,
        };
        PostOrRun([logger = access_logger_, entry = std::move(entry)]() { logger(entry); });
    }
}

void HttpSession::SendHttpError(int status_code, std::string_view reason) {
    auto resp = HttpResponse::Empty(status_code);
    PostOrSend(resp.FormatWireResponse(/*keep_alive=*/false));
    EmitAccessLog(status_code, 0);
    PostClose();
}

void HttpSession::SendUnaryResponse(const HttpResponse& resp) {
    bool is_head = (current_request_.Method() == HttpMethod::kHead);
    PostOrSend(resp.FormatWireResponse(current_request_.KeepAlive(), is_head));
    EmitAccessLog(resp.StatusCode(), resp.Body().size());
    if (current_request_.KeepAlive()) {
        Reset();
    } else {
        PostClose();
    }
}

void HttpSession::DispatchRequest() {
    if (!router_) {
        SendHttpError(static_cast<int>(HttpStatus::kNotFound),
                      HttpStatusToReason(HttpStatus::kNotFound));
        return;
    }

    std::string_view raw_path = current_request_.Path();
    size_t cut = raw_path.find_first_of("?#");
    std::string_view match_path =
            (cut == std::string_view::npos) ? raw_path : raw_path.substr(0, cut);

    if (const RouteHandler* match = router_->MatchHandler(current_request_.Method(), match_path)) {
        switch (match->kind) {
        case RouteHandler::Kind::kUnary:
            SendUnaryResponse(match->unary_handler(current_request_));
            return;
        case RouteHandler::Kind::kStreaming: {
            is_streaming_active_ = true;
            auto writer = std::make_shared<ResponseWriterImpl>(
                    shared_from_this(), current_request_.Method(),
                    std::string(current_request_.Path()), request_start_time_,
                    current_request_.KeepAlive());
            match->streaming_handler(current_request_, std::move(writer));
            return;
        }
        }
    }

    // Check for 405 Method Not Allowed
    auto allowed_methods = router_->GetAllowedMethods(match_path);
    if (!allowed_methods.empty()) {
        SendUnaryResponse(
                HttpResponse::Empty(HttpStatus::kMethodNotAllowed)
                        .WithHeader(headers::kAllow, absl::StrJoin(allowed_methods, ", ")));
        return;
    }

    // 404 Not Found
    if (not_found_handler_) {
        SendUnaryResponse(not_found_handler_(current_request_));
        return;
    }

    SendHttpError(static_cast<int>(HttpStatus::kNotFound),
                  HttpStatusToReason(HttpStatus::kNotFound));
}

int HttpSession::OnMessageBegin(llhttp_t* parser) {
    auto* self = static_cast<HttpSession*>(parser->data);
    self->Reset();
    return 0;
}

int HttpSession::AppendHeaderBytes(std::string& target, const char* at, size_t length) {
    if (current_headers_bytes_ + length > kMaxHeaderSize) {
        return HPE_USER;
    }
    current_headers_bytes_ += length;
    target.append(at, length);
    return 0;
}

int HttpSession::OnUrl(llhttp_t* parser, const char* at, size_t length) {
    auto* self = static_cast<HttpSession*>(parser->data);
    return self->AppendHeaderBytes(self->current_request_.path_, at, length);
}

int HttpSession::OnHeaderField(llhttp_t* parser, const char* at, size_t length) {
    auto* self = static_cast<HttpSession*>(parser->data);
    return self->AppendHeaderBytes(self->current_header_field_, at, length);
}

int HttpSession::OnHeaderValue(llhttp_t* parser, const char* at, size_t length) {
    auto* self = static_cast<HttpSession*>(parser->data);
    return self->AppendHeaderBytes(self->current_header_value_, at, length);
}

int HttpSession::OnHeaderValueComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpSession*>(parser->data);
    self->CommitCurrentHeader();
    return 0;
}

int HttpSession::OnHeadersComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpSession*>(parser->data);
    self->CommitCurrentHeader();

    const auto parsed_method = static_cast<llhttp_method_t>(parser->method);
    self->current_request_.method_ =
            internal::LlhttpMethodToHttpMethod(parsed_method).value_or(HttpMethod::kGet);
    self->current_request_.keep_alive_ = (llhttp_should_keep_alive(parser) != 0);

    if (parser->content_length > self->max_payload_size_) {
        self->payload_too_large_ = true;
        return HPE_USER;
    }

    return 0;
}

int HttpSession::OnBody(llhttp_t* parser, const char* at, size_t length) {
    auto* self = static_cast<HttpSession*>(parser->data);
    if (self->current_request_.body_.size() + length > self->max_payload_size_) {
        self->payload_too_large_ = true;
        return HPE_USER;
    }
    self->current_request_.body_.append(at, length);
    return 0;
}

int HttpSession::OnMessageComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpSession*>(parser->data);
    self->current_request_.keep_alive_ = (llhttp_should_keep_alive(parser) != 0);
    self->DispatchRequest();
    if (self->is_streaming_active_) {
        return HPE_PAUSED;
    }
    return 0;
}

}  // namespace goldfish::http
