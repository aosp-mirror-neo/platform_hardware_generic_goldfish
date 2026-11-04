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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/strings/match.h"
#include "absl/synchronization/notification.h"

#include "goldfish/async/testing/fake_async_socket.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/http/http_headers.h"
#include "goldfish/http/http_router.h"
#include "goldfish/network/endpoint.h"
#include "http_session_internal.h"

namespace goldfish::http {

using ::testing::_;

class TestAsyncSocket : public async::testing::FakeAsyncSocket {
  public:
    std::string sent_data;
    bool is_closed = false;

    TestAsyncSocket() {
        ON_CALL(*this, Send(testing::_, testing::_, testing::_))
                .WillByDefault([this](const char* buf, size_t len, OnSendCallback on_send) {
                    sent_data.append(buf, len);
                    if (on_send) {
                        on_send(absl::OkStatus());
                    }
                    return absl::OkStatus();
                });
        ON_CALL(*this, Close()).WillByDefault([this]() { is_closed = true; });
    }
};

class HttpSessionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fake_socket_ = std::make_shared<TestAsyncSocket>();
        router_ = std::make_unique<HttpRouter>();
    }

    std::shared_ptr<HttpSession> CreateSession(size_t max_payload_size = 64 * 1024 * 1024,
                                               HttpHandler not_found = nullptr) {
        return HttpSession::Create(fake_socket_, /*loop=*/nullptr, router_.get(), max_payload_size,
                                   std::move(not_found));
    }

    std::string GetSentData() {
        std::string result = std::move(fake_socket_->sent_data);
        fake_socket_->sent_data.clear();
        return result;
    }

    std::shared_ptr<TestAsyncSocket> fake_socket_;
    std::unique_ptr<HttpRouter> router_;
};

TEST_F(HttpSessionTest, UnaryGetRequest) {
    router_->AddRoute(HttpMethod::kGet, "/hello", [](const HttpRequest& req) {
        EXPECT_EQ(req.Method(), HttpMethod::kGet);
        EXPECT_EQ(req.Path(), "/hello");
        return HttpResponse::String("Hello, World!", 200, "text/plain");
    });

    auto session = CreateSession();
    std::string raw_http = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    EXPECT_TRUE(session->OnDataReceived(raw_http));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::StrContains(
            sent, absl::StrCat(headers::kContentType, ": ", mime::kTextPlain, "\r\n")));
    EXPECT_TRUE(absl::StrContains(sent, absl::StrCat(headers::kContentLength, ": 13\r\n")));
    EXPECT_TRUE(absl::EndsWith(sent, "\r\n\r\nHello, World!"));
}

TEST_F(HttpSessionTest, UnaryHeadRequestOmitsBody) {
    router_->AddRoute(HttpMethod::kHead, "/data", [](const HttpRequest& req) {
        EXPECT_EQ(req.Method(), HttpMethod::kHead);
        return HttpResponse::String("PayloadData123", 200, "text/plain");
    });

    auto session = CreateSession();
    std::string raw_http = "HEAD /data HTTP/1.1\r\nHost: localhost\r\n\r\n";
    EXPECT_TRUE(session->OnDataReceived(raw_http));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::StrContains(
            sent, absl::StrCat(headers::kContentType, ": ", mime::kTextPlain, "\r\n")));
    EXPECT_TRUE(absl::StrContains(sent, absl::StrCat(headers::kContentLength, ": 14\r\n")));
    EXPECT_TRUE(absl::EndsWith(sent, "\r\n\r\n"));
    EXPECT_FALSE(absl::StrContains(sent, "PayloadData123"));
}

TEST_F(HttpSessionTest, UnaryPostWithBody) {
    router_->AddRoute(HttpMethod::kPost, "/echo", [](const HttpRequest& req) {
        EXPECT_EQ(req.Method(), HttpMethod::kPost);
        EXPECT_EQ(req.GetHeader("Content-Type"), "text/plain");
        EXPECT_EQ(req.Body(), "Ping Payload");
        return HttpResponse::String(std::string(req.Body()), 200);
    });

    auto session = CreateSession();
    std::string raw_http =
            "POST /echo HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 12\r\n\r\n"
            "Ping Payload";
    EXPECT_TRUE(session->OnDataReceived(raw_http));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::EndsWith(sent, "\r\n\r\nPing Payload"));
}

TEST_F(HttpSessionTest, ZeroLengthPostPayload) {
    router_->AddRoute(HttpMethod::kPost, "/empty-post", [](const HttpRequest& req) {
        EXPECT_TRUE(req.Body().empty());
        return HttpResponse::String("Received Empty", 200);
    });

    auto session = CreateSession();
    std::string raw_http =
            "POST /empty-post HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 0\r\n\r\n";
    EXPECT_TRUE(session->OnDataReceived(raw_http));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::EndsWith(sent, "\r\n\r\nReceived Empty"));
}

TEST_F(HttpSessionTest, KeepAliveSequentialRequests) {
    int counter = 0;
    router_->AddRoute(HttpMethod::kGet, "/count", [&counter](const HttpRequest&) {
        counter++;
        return HttpResponse::String(std::to_string(counter));
    });

    auto session = CreateSession();

    // First request
    EXPECT_TRUE(session->OnDataReceived("GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string sent1 = GetSentData();
    EXPECT_TRUE(absl::EndsWith(sent1, "\r\n\r\n1"));
    EXPECT_FALSE(fake_socket_->is_closed);

    // Second request on same session
    EXPECT_TRUE(session->OnDataReceived("GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string sent2 = GetSentData();
    EXPECT_TRUE(absl::EndsWith(sent2, "\r\n\r\n2"));
    EXPECT_FALSE(fake_socket_->is_closed);
}

TEST_F(HttpSessionTest, ConnectionCloseHeaderClosesSocket) {
    router_->AddRoute(HttpMethod::kGet, "/bye",
                      [](const HttpRequest&) { return HttpResponse::String("Goodbye"); });

    auto session = CreateSession();
    EXPECT_TRUE(session->OnDataReceived(
            "GET /bye HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::EndsWith(sent, "\r\n\r\nGoodbye"));
    EXPECT_TRUE(fake_socket_->is_closed);
}

TEST_F(HttpSessionTest, MethodNotAllowed405) {
    router_->AddRoute(HttpMethod::kPost, "/submit",
                      [](const HttpRequest&) { return HttpResponse::Empty(200); });

    auto session = CreateSession();
    EXPECT_TRUE(session->OnDataReceived("GET /submit HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 405 Method Not Allowed\r\n"));
    EXPECT_TRUE(absl::StrContains(sent, absl::StrCat(headers::kAllow, ": POST\r\n")));
}

TEST_F(HttpSessionTest, NotFound404DefaultAndCustom) {
    auto session = CreateSession();
    EXPECT_TRUE(session->OnDataReceived("GET /unknown HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 404 Not Found\r\n"));

    // Custom 404 handler
    auto custom_session = CreateSession(64 * 1024 * 1024, [](const HttpRequest& req) {
        return HttpResponse::String("Custom 404 for " + std::string(req.Path()), 404);
    });
    EXPECT_TRUE(custom_session->OnDataReceived("GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string custom_sent = GetSentData();
    EXPECT_TRUE(absl::EndsWith(custom_sent, "\r\n\r\nCustom 404 for /missing"));
}

TEST_F(HttpSessionTest, PayloadTooLarge413EarlyRejection) {
    router_->AddRoute(HttpMethod::kPost, "/upload",
                      [](const HttpRequest&) { return HttpResponse::Empty(200); });

    // Limit to 10 bytes
    auto session = CreateSession(/*max_payload_size=*/10);
    EXPECT_FALSE(session->OnDataReceived(
            "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n"));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 413 Payload Too Large\r\n"));
    EXPECT_TRUE(fake_socket_->is_closed);
}

TEST_F(HttpSessionTest, MalformedHttpReturns400BadRequest) {
    auto session = CreateSession();
    EXPECT_FALSE(session->OnDataReceived("INVALID_COMMAND / HTTP/1.1\r\n\r\n"));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 400 Bad Request\r\n"));
    EXPECT_TRUE(fake_socket_->is_closed);
}

TEST_F(HttpSessionTest, ChunkedHeaderIngestionAndDuplicateHeaderMerging) {
    router_->AddRoute(HttpMethod::kGet, "/headers", [](const HttpRequest& req) {
        EXPECT_EQ(req.GetHeader("Accept"), "text/html, application/json");
        EXPECT_EQ(req.GetHeader("X-Custom-Split"), "SplitValue");
        return HttpResponse::String("OK");
    });

    auto session = CreateSession();
    // Feed data in 2-byte fragments to simulate TCP fragmentation
    std::string raw =
            "GET /headers HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Accept: text/html\r\n"
            "Accept: application/json\r\n"
            "X-Custom-Split: SplitValue\r\n\r\n";

    for (size_t i = 0; i < raw.size(); i += 2) {
        session->OnDataReceived(raw.substr(i, 2));
    }

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::EndsWith(sent, "\r\n\r\nOK"));
}

TEST_F(HttpSessionTest, ChunkedTransferEncodingBody) {
    router_->AddRoute(HttpMethod::kPost, "/chunked", [](const HttpRequest& req) {
        EXPECT_EQ(req.Body(), "Wikipedia in chunks");
        return HttpResponse::String("Received");
    });

    auto session = CreateSession();
    std::string raw =
            "POST /chunked HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "4\r\nWiki\r\n"
            "6\r\npedia \r\n"
            "9\r\nin chunks\r\n"
            "0\r\n\r\n";

    EXPECT_TRUE(session->OnDataReceived(raw));
    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::EndsWith(sent, "\r\n\r\nReceived"));
}

TEST_F(HttpSessionTest, UrlQueryParametersRouteMatch) {
    router_->AddRoute(HttpMethod::kGet, "/search", [](const HttpRequest& req) {
        EXPECT_TRUE(absl::StartsWith(req.Path(), "/search?q=test"));
        return HttpResponse::String("Query OK");
    });

    auto session = CreateSession();
    EXPECT_TRUE(session->OnDataReceived(
            "GET /search?q=test&lang=en HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::EndsWith(sent, "\r\n\r\nQuery OK"));
}

TEST_F(HttpSessionTest, StreamingResponseWriter) {
    std::function<void()> client_closed_cb;
    router_->AddStreamingRoute(
            HttpMethod::kPost, "/stream",
            [&client_closed_cb](const HttpRequest& req,
                                std::shared_ptr<HttpResponseWriter> writer) {
                writer->SendHeaders({{"Content-Type", "application/octet-stream"}});
                writer->SendChunk("CHUNK_1");
                writer->SendChunk("CHUNK_2");
                writer->SetOnCloseCallback([&client_closed_cb]() {
                    if (client_closed_cb) {
                        client_closed_cb();
                    }
                });
                writer->Finish();
            });

    auto session = CreateSession();
    EXPECT_TRUE(session->OnDataReceived("POST /stream HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::StrContains(sent, "Content-Type: application/octet-stream\r\n"));
    EXPECT_TRUE(absl::StrContains(sent, "CHUNK_1"));
    EXPECT_TRUE(absl::StrContains(sent, "CHUNK_2"));
}

TEST_F(HttpSessionTest, StreamingChunkedKeepAliveSequentialRequests) {
    router_->AddStreamingRoute(
            HttpMethod::kPost, "/chunk-stream",
            [](const HttpRequest& req, std::shared_ptr<HttpResponseWriter> writer) {
                writer->SendHeaders({{"Content-Type", "application/grpc-web"},
                                     {"Transfer-Encoding", "chunked"}});
                writer->SendChunk("STREAM_PAYLOAD");
                writer->Finish();
            });

    router_->AddRoute(HttpMethod::kGet, "/subsequent",
                      [](const HttpRequest&) { return HttpResponse::String("Subsequent OK"); });

    auto session = CreateSession();

    // 1. Streaming Chunked Request
    EXPECT_TRUE(session->OnDataReceived("POST /chunk-stream HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string sent1 = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent1, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::StrContains(sent1, "Transfer-Encoding: chunked\r\n"));
    EXPECT_TRUE(absl::StrContains(sent1, "STREAM_PAYLOAD"));
    EXPECT_TRUE(absl::EndsWith(sent1, "0\r\n\r\n"));
    EXPECT_FALSE(fake_socket_->is_closed);

    // 2. Subsequent request on same connection
    EXPECT_TRUE(session->OnDataReceived("GET /subsequent HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    std::string sent2 = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent2, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::EndsWith(sent2, "\r\n\r\nSubsequent OK"));
    EXPECT_FALSE(fake_socket_->is_closed);
}

TEST_F(HttpSessionTest, StreamingOnCloseCallbackTriggeredOnDetach) {
    bool closed_triggered = false;
    router_->AddStreamingRoute(
            HttpMethod::kPost, "/stream",
            [&closed_triggered](const HttpRequest&, std::shared_ptr<HttpResponseWriter> writer) {
                writer->SetOnCloseCallback([&closed_triggered]() { closed_triggered = true; });
            });

    auto session = CreateSession();
    EXPECT_TRUE(session->OnDataReceived("POST /stream HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    EXPECT_FALSE(closed_triggered);
    session->DetachSocket();
    EXPECT_TRUE(closed_triggered);
}

TEST_F(HttpSessionTest, EmptyHeaderValuePreserved) {
    router_->AddRoute(HttpMethod::kGet, "/empty-header", [](const HttpRequest& req) {
        EXPECT_TRUE(req.HasHeader("X-Empty-Header"));
        EXPECT_EQ(req.GetHeader("X-Empty-Header"), "");
        EXPECT_TRUE(req.HasHeader("X-Normal-Header"));
        EXPECT_EQ(req.GetHeader("X-Normal-Header"), "NormalValue");
        return HttpResponse::String("OK");
    });

    auto session = CreateSession();
    std::string raw =
            "GET /empty-header HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "X-Empty-Header:\r\n"
            "X-Normal-Header: NormalValue\r\n\r\n";

    EXPECT_TRUE(session->OnDataReceived(raw));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::EndsWith(sent, "\r\n\r\nOK"));
}

TEST_F(HttpSessionTest, AccessLogRecordsClientEndpointAndDuration) {
    AccessLogEntry logged_entry;
    bool logged = false;

    auto test_endpoint = network::ToEndpoint(network::ToIpAddress("192.168.1.50").value(), 54321);

    router_->AddRoute(HttpMethod::kGet, "/ping",
                      [](const HttpRequest&) { return HttpResponse::String("pong"); });

    auto session = HttpSession::Create(
            fake_socket_, /*loop=*/nullptr, router_.get(),
            /*max_payload_size=*/64 * 1024 * 1024,
            /*not_found_handler=*/nullptr,
            /*access_logger=*/
            [&](const AccessLogEntry& entry) {
                logged_entry = entry;
                logged = true;
            },
            test_endpoint);

    EXPECT_TRUE(session->OnDataReceived("GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    EXPECT_TRUE(logged);
    EXPECT_EQ(logged_entry.client_endpoint, test_endpoint);
    EXPECT_EQ(logged_entry.method, HttpMethod::kGet);
    EXPECT_EQ(logged_entry.path, "/ping");
    EXPECT_EQ(logged_entry.status_code, 200);
    EXPECT_EQ(logged_entry.bytes_sent, 4);
    EXPECT_GE(logged_entry.duration, absl::ZeroDuration());
}

TEST_F(HttpSessionTest, StreamingChunkedTransferEncodingFraming) {
    router_->AddStreamingRoute(
            HttpMethod::kGet, "/chunked",
            [](const HttpRequest&, std::shared_ptr<HttpResponseWriter> writer) {
                writer->SendHeaders({{std::string(headers::kTransferEncoding), "chunked"}});
                writer->SendChunk("Hello");
                writer->SendChunk(" World!");
                writer->Finish();
            });

    auto session = CreateSession();
    EXPECT_TRUE(session->OnDataReceived("GET /chunked HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(sent.find("transfer-encoding: chunked\r\n"), std::string::npos);
    EXPECT_NE(sent.find("\r\n\r\n5\r\nHello\r\n7\r\n World!\r\n0\r\n\r\n"), std::string::npos);
}

TEST_F(HttpSessionTest, StreamingRawCloseDelimited) {
    router_->AddStreamingRoute(HttpMethod::kGet, "/raw-stream",
                               [](const HttpRequest&, std::shared_ptr<HttpResponseWriter> writer) {
                                   writer->SendHeaders({{std::string(headers::kContentType),
                                                         std::string(mime::kTextPlain)}});
                                   writer->SendChunk("Raw stream data");
                                   writer->Finish();
                               });

    auto session = CreateSession();
    EXPECT_TRUE(session->OnDataReceived("GET /raw-stream HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(sent.find("\r\n\r\nRaw stream data"), std::string::npos);
}

TEST_F(HttpSessionTest, StreamingPipelinedRequestMetadataIsolation) {
    std::vector<AccessLogEntry> logged_entries;
    std::shared_ptr<HttpResponseWriter> streaming_writer;

    router_->AddStreamingRoute(
            HttpMethod::kPost, "/stream-task",
            [&streaming_writer](const HttpRequest&, std::shared_ptr<HttpResponseWriter> writer) {
                streaming_writer = writer;
            });

    router_->AddRoute(HttpMethod::kGet, "/quick-unary",
                      [](const HttpRequest&) { return HttpResponse::String("quick result"); });

    auto session = HttpSession::Create(
            fake_socket_, /*loop=*/nullptr, router_.get(),
            /*max_payload_size=*/64 * 1024 * 1024,
            /*not_found_handler=*/nullptr,
            /*access_logger=*/[&](const AccessLogEntry& e) { logged_entries.push_back(e); });

    // 1. Dispatch Streaming Request #1
    EXPECT_TRUE(session->OnDataReceived(
            "POST /stream-task HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n"));
    ASSERT_NE(streaming_writer, nullptr);

    // 2. Dispatch Unary Request #2 immediately on same connection (Pipelining / Keep-Alive)
    EXPECT_TRUE(session->OnDataReceived("GET /quick-unary HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    // Request #2 is queued in pipelined buffer while Request #1 stream is in flight
    EXPECT_EQ(logged_entries.size(), 0);

    // 3. Complete Streaming Request #1 from background thread or later point
    streaming_writer->SendHeaders({{std::string(headers::kContentType), "text/plain"},
                                   {std::string(headers::kTransferEncoding), "chunked"}},
                                  200);
    streaming_writer->SendChunk("chunk payload");
    streaming_writer->Finish();

    // Verify both requests completed in strict FIFO order and retained metadata
    ASSERT_EQ(logged_entries.size(), 2);
    EXPECT_EQ(logged_entries[0].method, HttpMethod::kPost);
    EXPECT_EQ(logged_entries[0].path, "/stream-task");
    EXPECT_EQ(logged_entries[0].status_code, 200);
    EXPECT_EQ(logged_entries[0].bytes_sent, 13);

    EXPECT_EQ(logged_entries[1].method, HttpMethod::kGet);
    EXPECT_EQ(logged_entries[1].path, "/quick-unary");
    EXPECT_EQ(logged_entries[1].status_code, 200);
}

TEST_F(HttpSessionTest, ErrorTeardownPreventsSubsequentExecution) {
    auto session = CreateSession();

    // Send malformed HTTP request
    std::string malformed = "INVALID_VERB /test HTTP/1.1\r\n\r\n";
    EXPECT_FALSE(session->OnDataReceived(malformed));

    // Verify 400 Bad Request was sent
    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 400 Bad Request\r\n"));

    // Subsequent data feeds on the faulted session must be immediately rejected
    EXPECT_FALSE(session->OnDataReceived("GET /valid HTTP/1.1\r\nHost: localhost\r\n\r\n"));
}

TEST_F(HttpSessionTest, DetachSocketDestructorSafety) {
    bool detached_called = false;
    bool client_closed_called = false;

    {
        auto session = CreateSession();
        session->SetOnDetachedCallback([&detached_called](const std::shared_ptr<HttpSession>& s) {
            if (s) {
                detached_called = true;
            }
        });

        router_->AddStreamingRoute(
                HttpMethod::kPost, "/stream",
                [&client_closed_called](const HttpRequest&, std::shared_ptr<HttpResponseWriter> w) {
                    w->SetOnCloseCallback(
                            [&client_closed_called]() { client_closed_called = true; });
                });

        EXPECT_TRUE(session->OnDataReceived("POST /stream HTTP/1.1\r\nHost: localhost\r\n\r\n"));
        // session falls out of scope here without explicit DetachSocket() call
    }

    EXPECT_TRUE(client_closed_called);
}

TEST_F(HttpSessionTest, PipeliningResponseOrderViolation) {
    std::shared_ptr<HttpResponseWriter> stream_writer;
    absl::Notification stream_started;

    router_->AddStreamingRoute(HttpMethod::kPost, "/stream",
                               [&](const HttpRequest&, std::shared_ptr<HttpResponseWriter> writer) {
                                   stream_writer = writer;
                                   stream_started.Notify();
                               });

    router_->AddRoute(HttpMethod::kGet, "/unary",
                      [](const HttpRequest&) { return HttpResponse::String("Unary Response"); });

    auto session = CreateSession();

    // Send both requests Pipelined
    std::string raw_http =
            "POST /stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n"
            "GET /unary HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // llhttp processes both requests synchronously in sequence
    EXPECT_TRUE(session->OnDataReceived(raw_http));

    // Wait for the stream handler to be triggered.
    EXPECT_TRUE(stream_started.HasBeenNotified());

    // Send streaming headers and chunk later (simulate async activity)
    ASSERT_NE(stream_writer, nullptr);
    stream_writer->SendHeaders(
            {{"Content-Type", "text/plain"}, {std::string(headers::kTransferEncoding), "chunked"}},
            200);
    stream_writer->SendChunk("Stream Data Chunk");
    stream_writer->Finish();

    std::string sent = GetSentData();

    // Check if the fast Unary Response appeared BEFORE the streaming response
    size_t pos_stream = sent.find("Stream Data Chunk");
    size_t pos_unary = sent.find("Unary Response");

    ASSERT_NE(pos_stream, std::string::npos);
    ASSERT_NE(pos_unary, std::string::npos);

    // This assertion will FAIL if the bug exists
    EXPECT_LT(pos_stream, pos_unary)
            << "Responses are out of order! Fast Unary Response was sent before "
            << "the preceding active Streaming Response finished.";
}

TEST_F(HttpSessionTest, SendChunkAfterFinishCorruptsKeepAlive) {
    std::shared_ptr<HttpResponseWriter> stream_writer;

    router_->AddStreamingRoute(
            HttpMethod::kPost, "/stream",
            [&](const HttpRequest&, std::shared_ptr<HttpResponseWriter> writer) {
                stream_writer = writer;
                writer->SendHeaders({{std::string(headers::kTransferEncoding), "chunked"}});
                writer->SendChunk("First Chunk");
                writer->Finish();
                // writer is retained and not cleared!
            });

    router_->AddRoute(HttpMethod::kGet, "/next",
                      [](const HttpRequest&) { return HttpResponse::String("Next OK"); });

    auto session = CreateSession();

    // 1. Process first request
    EXPECT_TRUE(session->OnDataReceived("POST /stream HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    // Clear sent buffer for clean assessment
    GetSentData();

    // 2. Client sends another request on the same Keep-Alive connection
    EXPECT_TRUE(session->OnDataReceived("GET /next HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    // 3. Rogue background thread calls SendChunk after Finish() has already reset parsing
    ASSERT_NE(stream_writer, nullptr);
    stream_writer->SendChunk("Stale Stains");

    // 4. Verify that the sent data didn't get interspersed with garbage
    std::string sent = GetSentData();

    // This assertion will FAIL if the bug exists
    EXPECT_FALSE(absl::StrContains(sent, "Stale Stains"))
            << "Rogue SendChunk() reached the socket after Finish() and corrupted Keep-Alive "
               "stream!";
}

TEST_F(HttpSessionTest, PipelinedBufferLimitDuringStreaming) {
    std::shared_ptr<HttpResponseWriter> stream_writer;
    router_->AddStreamingRoute(HttpMethod::kPost, "/stream",
                               [&](const HttpRequest&, std::shared_ptr<HttpResponseWriter> writer) {
                                   stream_writer = writer;
                               });

    // Create session with 1 KiB max payload size
    auto session = CreateSession(1024);

    EXPECT_TRUE(session->OnDataReceived("POST /stream HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    ASSERT_NE(stream_writer, nullptr);

    // Flood pipelined buffer beyond 1024 bytes while stream is active
    std::string excessive_data(2048, 'A');
    EXPECT_FALSE(session->OnDataReceived(excessive_data));

    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StrContains(sent, "413 Payload Too Large"));
}

TEST_F(HttpSessionTest, OversizedHeadersRejected) {
    auto session = CreateSession();
    std::string large_header_val(70 * 1024, 'X');
    std::string req = absl::StrCat(
            "GET / HTTP/1.1\r\nHost: localhost\r\nX-Large: ", large_header_val, "\r\n\r\n");
    EXPECT_FALSE(session->OnDataReceived(req));
    std::string sent = GetSentData();
    EXPECT_TRUE(absl::StrContains(sent, "400 Bad Request"));
}

TEST_F(HttpSessionTest, SetOnCloseCallbackFiresImmediatelyWhenAlreadyDetached) {
    std::shared_ptr<HttpResponseWriter> captured_writer;
    router_->AddStreamingRoute(HttpMethod::kPost, "/stream",
                               [&](const HttpRequest&, std::shared_ptr<HttpResponseWriter> writer) {
                                   captured_writer = writer;
                               });

    auto session = CreateSession();
    EXPECT_TRUE(session->OnDataReceived("POST /stream HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    ASSERT_NE(captured_writer, nullptr);

    // Detach socket before SetOnCloseCallback is registered
    session->DetachSocket();

    bool callback_fired = false;
    captured_writer->SetOnCloseCallback([&]() { callback_fired = true; });
    EXPECT_TRUE(callback_fired);
}

TEST_F(HttpSessionTest, LlhttpMethodToHttpMethodMatchesStringParsing) {
    for (int m = HTTP_DELETE; m <= HTTP_QUERY; ++m) {
        const auto method = static_cast<llhttp_method_t>(m);
        const char* name = llhttp_method_name(method);
        ASSERT_NE(name, nullptr);
        EXPECT_EQ(internal::LlhttpMethodToHttpMethod(method), StringToHttpMethod(name))
                << "llhttp_method=" << m << " (" << name << ")";
    }
    EXPECT_FALSE(internal::LlhttpMethodToHttpMethod(static_cast<llhttp_method_t>(HTTP_QUERY + 1))
                         .has_value());
    EXPECT_FALSE(internal::LlhttpMethodToHttpMethod(static_cast<llhttp_method_t>(HTTP_QUERY + 16))
                         .has_value());
}

TEST_F(HttpSessionTest, DispatchesAllSupportedHttpMethods) {
    struct MethodCase {
        HttpMethod method;
        std::string_view raw_request;
    };
    constexpr MethodCase kCases[] = {
        {HttpMethod::kPut,
         "PUT /resource HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n"},
        {HttpMethod::kDelete, "DELETE /resource HTTP/1.1\r\nHost: localhost\r\n\r\n"},
        {HttpMethod::kOptions, "OPTIONS /resource HTTP/1.1\r\nHost: localhost\r\n\r\n"},
        {HttpMethod::kPatch,
         "PATCH /resource HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n"},
    };

    for (const auto& tc : kCases) {
        router_->AddRoute(tc.method, "/resource", [expected = tc.method](const HttpRequest& req) {
            EXPECT_EQ(req.Method(), expected);
            return HttpResponse::String(std::string(HttpMethodToString(req.Method())));
        });
    }

    auto session = CreateSession();
    for (const auto& tc : kCases) {
        EXPECT_TRUE(session->OnDataReceived(tc.raw_request));
        std::string sent = GetSentData();
        EXPECT_TRUE(absl::StartsWith(sent, "HTTP/1.1 200 OK\r\n"));
        EXPECT_TRUE(absl::EndsWith(sent, absl::StrCat("\r\n\r\n", HttpMethodToString(tc.method))));
    }
}

TEST_F(HttpSessionTest, FormatStreamingHeadersDetectsCaseInsensitiveChunked) {
    const absl::flat_hash_map<std::string, std::string> chunked_headers = {
        {std::string(headers::kContentType), std::string(mime::kApplicationGrpcWebProto)},
        {"TrAnSfEr-EnCoDiNg", "ChUnKeD"},
    };
    bool is_chunked = false;
    const std::string wire = internal::FormatStreamingHeaders(chunked_headers, 200, &is_chunked);
    EXPECT_TRUE(is_chunked);
    EXPECT_TRUE(absl::StartsWith(wire, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::StrContains(wire, "content-type: application/grpc-web+proto\r\n"));
    EXPECT_TRUE(absl::StrContains(wire, "TrAnSfEr-EnCoDiNg: ChUnKeD\r\n"));
    EXPECT_TRUE(absl::EndsWith(wire, "\r\n\r\n"));

    const absl::flat_hash_map<std::string, std::string> raw_headers = {
        {std::string(headers::kContentType), std::string(mime::kTextPlain)},
        {"X-Device-Name", "Pixel_9_Pro_エミュレータ"},
    };
    bool raw_chunked = true;
    const std::string raw_wire = internal::FormatStreamingHeaders(raw_headers, 201, &raw_chunked);
    EXPECT_FALSE(raw_chunked);
    EXPECT_TRUE(absl::StartsWith(raw_wire, "HTTP/1.1 201 Created\r\n"));
    EXPECT_TRUE(absl::StrContains(raw_wire, "X-Device-Name: Pixel_9_Pro_エミュレータ\r\n"));
    EXPECT_TRUE(absl::EndsWith(raw_wire, "\r\n\r\n"));
}

TEST_F(HttpSessionTest, ResetClearsStateOnLoopAndOffLoopThread) {
    std::vector<AccessLogEntry> logged_entries;
    router_->AddRoute(HttpMethod::kGet, "/first", [](const HttpRequest& req) {
        EXPECT_EQ(req.GetHeader("X-First-Header"), "first-val");
        return HttpResponse::String("first", 200, "text/plain");
    });
    router_->AddRoute(HttpMethod::kGet, "/second", [](const HttpRequest& req) {
        // Verify no headers or body leaked from the previous request after Reset().
        EXPECT_FALSE(req.HasHeader("X-First-Header"));
        EXPECT_TRUE(req.Body().empty());
        return HttpResponse::String("second", 200, "text/plain");
    });

    auto loop = async::testing::TestEventLoop::Create("HttpSessionResetLoop");
    auto session = HttpSession::Create(fake_socket_, loop.get(), router_.get(),
                                       /*max_payload_size=*/64 * 1024 * 1024,
                                       /*not_found_handler=*/nullptr,
                                       /*access_logger=*/[&](const AccessLogEntry& entry) {
                                           logged_entries.push_back(entry);
                                       });

    // 1. Process first Keep-Alive request and invoke Reset() directly on the
    // loop thread (IsOnLoopThread() == true), verifying no extra task is queued.
    ASSERT_TRUE(loop->Post([&]() {
                        EXPECT_TRUE(
                                session->OnDataReceived("GET /first HTTP/1.1\r\n"
                                                        "Host: localhost\r\n"
                                                        "X-First-Header: first-val\r\n\r\n"));
                        session->Reset();
                    }).ok());
    loop->RunAll();
    EXPECT_EQ(loop->TaskCount(), 0u);
    std::string first_sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(first_sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::EndsWith(first_sent, "\r\n\r\nfirst"));

    // 2. Calling Reset() from the test thread (IsOnLoopThread() == false) must
    // post ResetDirect() onto the EventLoop rather than mutating state off-loop.
    session->Reset();
    EXPECT_EQ(loop->TaskCount(), 1u);
    loop->RunAll();
    EXPECT_EQ(loop->TaskCount(), 0u);

    // 3. Verify a subsequent Keep-Alive request parses cleanly and records a
    // valid non-negative duration in AccessLogger.
    ASSERT_TRUE(loop->Post([&]() {
                        EXPECT_TRUE(session->OnDataReceived(
                                "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n"));
                    }).ok());
    loop->RunAll();

    std::string second_sent = GetSentData();
    EXPECT_TRUE(absl::StartsWith(second_sent, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::EndsWith(second_sent, "\r\n\r\nsecond"));
    ASSERT_EQ(logged_entries.size(), 2u);
    EXPECT_EQ(logged_entries[0].method, HttpMethod::kGet);
    EXPECT_EQ(logged_entries[0].path, "/first");
    EXPECT_EQ(logged_entries[0].status_code, 200);
    EXPECT_GE(logged_entries[0].duration, absl::ZeroDuration());
    EXPECT_EQ(logged_entries[1].method, HttpMethod::kGet);
    EXPECT_EQ(logged_entries[1].path, "/second");
    EXPECT_EQ(logged_entries[1].status_code, 200);
    EXPECT_GE(logged_entries[1].duration, absl::ZeroDuration());
}

TEST_F(HttpSessionTest, CommitCurrentHeaderInsertsUniqueAndAppendsCaseInsensitiveDuplicates) {
    bool handler_called = false;
    router_->AddRoute(HttpMethod::kGet, "/headers", [&](const HttpRequest& req) {
        handler_called = true;
        EXPECT_EQ(req.GetHeader("Accept"), "application/json, text/plain; charset=utf-8");
        EXPECT_EQ(req.GetHeader("accept"), "application/json, text/plain; charset=utf-8");
        EXPECT_EQ(req.GetHeader("X-Emulator-Label"), "Pixel_9_Pro_エミュレータ");
        EXPECT_EQ(req.GetHeader("X-Empty"), "");
        return HttpResponse::String("ok", 200, "text/plain");
    });

    auto session = CreateSession();
    EXPECT_TRUE(
            session->OnDataReceived("GET /headers HTTP/1.1\r\n"
                                    "Host: localhost\r\n"
                                    "Accept: application/json\r\n"
                                    "ACCEPT: text/plain; charset=utf-8\r\n"
                                    "X-Emulator-Label: Pixel_9_Pro_エミュレータ\r\n"
                                    "X-Empty: \r\n\r\n"));
    EXPECT_TRUE(handler_called);
}

}  // namespace goldfish::http
