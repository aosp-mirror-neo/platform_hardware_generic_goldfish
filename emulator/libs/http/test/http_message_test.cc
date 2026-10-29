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
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/strings/match.h"

#include "goldfish/http/http_request.h"
#include "goldfish/http/http_response.h"

namespace goldfish::http {

class HttpRequestTestPeer {
  public:
    static void SetRequest(HttpRequest* req, HttpMethod method, std::string path,
                           HttpRequest::HeaderMap headers, std::string body,
                           bool keep_alive = true) {
        req->method_ = method;
        req->path_ = std::move(path);
        req->headers_ = std::move(headers);
        req->body_ = std::move(body);
        req->keep_alive_ = keep_alive;
    }
};

TEST(HttpMessageTest, HttpHeadersAndMimeConstants) {
    EXPECT_EQ(headers::kAccept, "accept");
    EXPECT_EQ(headers::kAuthorization, "authorization");
    EXPECT_EQ(headers::kContentType, "content-type");
    EXPECT_EQ(headers::kContentLength, "content-length");
    EXPECT_EQ(headers::kConnection, "connection");
    EXPECT_EQ(headers::kHost, "host");
    EXPECT_EQ(headers::kOrigin, "origin");
    EXPECT_EQ(headers::kAccessControlAllowOrigin, "access-control-allow-origin");
    EXPECT_EQ(headers::kAccessControlAllowMethods, "access-control-allow-methods");
    EXPECT_EQ(headers::kXGrpcWeb, "x-grpc-web");

    EXPECT_EQ(mime::kApplicationJson, "application/json");
    EXPECT_EQ(mime::kApplicationOctetStream, "application/octet-stream");
    EXPECT_EQ(mime::kApplicationGrpcWeb, "application/grpc-web");
    EXPECT_EQ(mime::kApplicationGrpcWebProto, "application/grpc-web+proto");
    EXPECT_EQ(mime::kTextPlain, "text/plain");

    EXPECT_EQ(methods::kGet, "GET");
    EXPECT_EQ(methods::kPost, "POST");
    EXPECT_EQ(methods::kPut, "PUT");
    EXPECT_EQ(methods::kDelete, "DELETE");
    EXPECT_EQ(methods::kOptions, "OPTIONS");
    EXPECT_EQ(methods::kHead, "HEAD");
    EXPECT_EQ(methods::kPatch, "PATCH");
    EXPECT_EQ(methods::kAny, "*");

    EXPECT_EQ(protocol::kHttp11, "HTTP/1.1");
    EXPECT_EQ(protocol::kCrlf, "\r\n");
    EXPECT_EQ(protocol::kDoubleCrlf, "\r\n\r\n");
    EXPECT_EQ(protocol::kColonSpace, ": ");
    EXPECT_EQ(protocol::kSpace, " ");
    EXPECT_EQ(protocol::kKeepAlive, "keep-alive");
    EXPECT_EQ(protocol::kClose, "close");
}

TEST(HttpMessageTest, HttpStatusConstantsAndReasonPhrases) {
    EXPECT_EQ(static_cast<int>(HttpStatus::kOk), 200);
    EXPECT_EQ(static_cast<int>(HttpStatus::kCreated), 201);
    EXPECT_EQ(static_cast<int>(HttpStatus::kNoContent), 204);
    EXPECT_EQ(static_cast<int>(HttpStatus::kBadRequest), 400);
    EXPECT_EQ(static_cast<int>(HttpStatus::kUnauthorized), 401);
    EXPECT_EQ(static_cast<int>(HttpStatus::kForbidden), 403);
    EXPECT_EQ(static_cast<int>(HttpStatus::kNotFound), 404);
    EXPECT_EQ(static_cast<int>(HttpStatus::kMethodNotAllowed), 405);
    EXPECT_EQ(static_cast<int>(HttpStatus::kPayloadTooLarge), 413);
    EXPECT_EQ(static_cast<int>(HttpStatus::kUnsupportedMediaType), 415);
    EXPECT_EQ(static_cast<int>(HttpStatus::kInternalServerError), 500);
    EXPECT_EQ(static_cast<int>(HttpStatus::kServiceUnavailable), 503);

    EXPECT_EQ(HttpStatusToReason(HttpStatus::kOk), "OK");
    EXPECT_EQ(HttpStatusToReason(HttpStatus::kCreated), "Created");
    EXPECT_EQ(HttpStatusToReason(HttpStatus::kNoContent), "No Content");
    EXPECT_EQ(HttpStatusToReason(HttpStatus::kBadRequest), "Bad Request");
    EXPECT_EQ(HttpStatusToReason(HttpStatus::kNotFound), "Not Found");
    EXPECT_EQ(HttpStatusToReason(HttpStatus::kMethodNotAllowed), "Method Not Allowed");
    EXPECT_EQ(HttpStatusToReason(HttpStatus::kPayloadTooLarge), "Payload Too Large");
    EXPECT_EQ(HttpStatusToReason(HttpStatus::kInternalServerError), "Internal Server Error");
    EXPECT_EQ(HttpStatusToReason(999), "Unknown Status");

    auto resp = HttpResponse::String("Created Item", HttpStatus::kCreated);
    EXPECT_EQ(resp.Status(), HttpStatus::kCreated);
    EXPECT_EQ(resp.StatusCode(), 201);
}

TEST(HttpMessageTest, MethodConversions) {
    EXPECT_EQ(HttpMethodToString(HttpMethod::kGet), "GET");
    EXPECT_EQ(HttpMethodToString(HttpMethod::kPost), "POST");
    EXPECT_EQ(HttpMethodToString(HttpMethod::kPut), "PUT");
    EXPECT_EQ(HttpMethodToString(HttpMethod::kDelete), "DELETE");
    EXPECT_EQ(HttpMethodToString(HttpMethod::kOptions), "OPTIONS");
    EXPECT_EQ(HttpMethodToString(HttpMethod::kHead), "HEAD");
    EXPECT_EQ(HttpMethodToString(HttpMethod::kPatch), "PATCH");
    EXPECT_EQ(HttpMethodToString(HttpMethod::kAny), "*");

    EXPECT_EQ(StringToHttpMethod("GET"), HttpMethod::kGet);
    EXPECT_EQ(StringToHttpMethod("get"), HttpMethod::kGet);
    EXPECT_EQ(StringToHttpMethod("gEt"), HttpMethod::kGet);
    EXPECT_EQ(StringToHttpMethod("POST"), HttpMethod::kPost);
    EXPECT_EQ(StringToHttpMethod("post"), HttpMethod::kPost);
    EXPECT_EQ(StringToHttpMethod("pOsT"), HttpMethod::kPost);
    EXPECT_EQ(StringToHttpMethod("PUT"), HttpMethod::kPut);
    EXPECT_EQ(StringToHttpMethod("put"), HttpMethod::kPut);
    EXPECT_EQ(StringToHttpMethod("pUt"), HttpMethod::kPut);
    EXPECT_EQ(StringToHttpMethod("OPTIONS"), HttpMethod::kOptions);
    EXPECT_EQ(StringToHttpMethod("options"), HttpMethod::kOptions);
    EXPECT_EQ(StringToHttpMethod("oPtIoNs"), HttpMethod::kOptions);
    EXPECT_EQ(StringToHttpMethod("DELETE"), HttpMethod::kDelete);
    EXPECT_EQ(StringToHttpMethod("delete"), HttpMethod::kDelete);
    EXPECT_EQ(StringToHttpMethod("dElEtE"), HttpMethod::kDelete);
    EXPECT_EQ(StringToHttpMethod("HEAD"), HttpMethod::kHead);
    EXPECT_EQ(StringToHttpMethod("head"), HttpMethod::kHead);
    EXPECT_EQ(StringToHttpMethod("hEaD"), HttpMethod::kHead);
    EXPECT_EQ(StringToHttpMethod("PATCH"), HttpMethod::kPatch);
    EXPECT_EQ(StringToHttpMethod("patch"), HttpMethod::kPatch);
    EXPECT_EQ(StringToHttpMethod("pAtCh"), HttpMethod::kPatch);
    EXPECT_EQ(StringToHttpMethod("*"), HttpMethod::kAny);

    // Reject empty, unsupported lengths, and unknown verbs of matching lengths.
    EXPECT_FALSE(StringToHttpMethod("").has_value());
    EXPECT_FALSE(StringToHttpMethod("?").has_value());
    EXPECT_FALSE(StringToHttpMethod("GO").has_value());
    EXPECT_FALSE(StringToHttpMethod("PRI").has_value());
    EXPECT_FALSE(StringToHttpMethod("LOCK").has_value());
    EXPECT_FALSE(StringToHttpMethod("TRACE").has_value());
    EXPECT_FALSE(StringToHttpMethod("UNLINK").has_value());
    EXPECT_FALSE(StringToHttpMethod("INVALID").has_value());
    EXPECT_FALSE(StringToHttpMethod("CONNECT").has_value());
    EXPECT_FALSE(StringToHttpMethod("PROPFIND").has_value());

    // Reject ASCII boundary bytes ('@'=0x40, '['=0x5B, '`'=0x60, '{'=0x7B), embedded NULs,
    // and non-ASCII UTF-8 sequences of matching byte lengths.
    EXPECT_FALSE(StringToHttpMethod("G@T").has_value());
    EXPECT_FALSE(StringToHttpMethod("G[T").has_value());
    EXPECT_FALSE(StringToHttpMethod("G`T").has_value());
    EXPECT_FALSE(StringToHttpMethod("G{T").has_value());
    EXPECT_FALSE(StringToHttpMethod(std::string_view("G\0T", 3)).has_value());
    EXPECT_FALSE(StringToHttpMethod(std::string_view("GET\0", 4)).has_value());
    EXPECT_FALSE(StringToHttpMethod("\xE2\x82\xAC").has_value());   // 3-byte UTF-8 '€'
    EXPECT_FALSE(StringToHttpMethod("G\xC3\x89T").has_value());     // 4-byte UTF-8 "GÉT"
    EXPECT_FALSE(StringToHttpMethod("P\xC3\x96ST").has_value());    // 5-byte UTF-8 "PÖST"
    EXPECT_FALSE(StringToHttpMethod("D\xC3\x89LETE").has_value());  // 7-byte UTF-8 "DÉLETE"
}

TEST(HttpMessageTest, HttpRequestCaseInsensitiveHeaders) {
    HttpRequest req;
    HttpRequestTestPeer::SetRequest(
            &req, HttpMethod::kPost, "/api/test",
            {{"content-type", "application/json"}, {"authorization", "Bearer token123"}},
            "{\"key\":\"value\"}", true);

    EXPECT_EQ(req.Method(), HttpMethod::kPost);
    EXPECT_EQ(req.Path(), "/api/test");
    EXPECT_EQ(req.Body(), "{\"key\":\"value\"}");
    EXPECT_TRUE(req.KeepAlive());

    EXPECT_EQ(req.GetHeader("Content-Type"), "application/json");
    EXPECT_EQ(req.GetHeader("content-type"), "application/json");
    EXPECT_EQ(req.GetHeader("CONTENT-TYPE"), "application/json");
    EXPECT_TRUE(req.HasHeader("Content-Type"));
    EXPECT_TRUE(req.HasHeader("content-type"));
    EXPECT_TRUE(req.HasHeader("CONTENT-TYPE"));
    EXPECT_EQ(req.GetHeader("Authorization"), "Bearer token123");
    EXPECT_TRUE(req.HasHeader("Authorization"));
    EXPECT_FALSE(req.HasHeader("Non-Existent"));
    EXPECT_FALSE(req.HasHeader(""));
    EXPECT_EQ(req.GetHeader("Non-Existent"), "");
    EXPECT_EQ(req.GetHeader(""), "");
}

TEST(HttpMessageTest, HttpResponseStringFactoryAndWireFormatting) {
    auto resp = HttpResponse::String("Hello, World!", HttpStatus::kOk, mime::kTextPlain)
                        .WithHeader("X-Custom", "abc");

    EXPECT_EQ(resp.Status(), HttpStatus::kOk);
    EXPECT_EQ(resp.StatusCode(), 200);
    EXPECT_EQ(resp.Body(), "Hello, World!");
    EXPECT_EQ(resp.Headers().at(std::string(headers::kContentType)), mime::kTextPlain);
    EXPECT_EQ(resp.Headers().at("X-Custom"), "abc");

    std::string wire = resp.FormatWireResponse(/*keep_alive=*/true);
    EXPECT_TRUE(absl::StartsWith(wire, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::StrContains(
            wire, absl::StrCat(headers::kContentType, ": ", mime::kTextPlain, "\r\n")));
    EXPECT_TRUE(absl::StrContains(wire, absl::StrCat(headers::kContentLength, ": 13\r\n")));
    EXPECT_TRUE(absl::StrContains(wire, absl::StrCat(headers::kConnection, ": keep-alive\r\n")));
    EXPECT_TRUE(absl::StrContains(wire, "X-Custom: abc\r\n"));
    EXPECT_TRUE(absl::EndsWith(wire, "\r\n\r\nHello, World!"));
}

TEST(HttpMessageTest, HttpResponseHeaderDeduplicationCaseInsensitive) {
    auto resp = HttpResponse::String("Deduplicated", HttpStatus::kOk)
                        .WithHeader("content-length", "100")
                        .WithHeader("Content-Length", "200")
                        .WithHeader("CONNECTION", "upgrade");

    EXPECT_EQ(resp.Headers().size(), 3);  // Content-Type, Content-Length, Connection
    EXPECT_EQ(resp.Headers().find("content-length")->second, "200");
    EXPECT_EQ(resp.Headers().find("CONTENT-LENGTH")->second, "200");

    std::string wire = resp.FormatWireResponse(/*keep_alive=*/true);
    // Should NOT emit duplicate Content-Length or Connection headers
    EXPECT_TRUE(absl::StrContains(wire, "content-length: 200\r\n") ||
                absl::StrContains(wire, "Content-Length: 200\r\n"));
    EXPECT_FALSE(absl::StrContains(wire, "100\r\n"));
    EXPECT_TRUE(absl::StrContains(wire, "CONNECTION: upgrade\r\n"));
    EXPECT_FALSE(absl::StrContains(wire, "Connection: keep-alive\r\n"));
}

TEST(HttpMessageTest, HttpResponseBinaryPayloadWithNullBytes) {
    std::string binary_data = "prefix\0\0\0suffix";
    binary_data.assign("pre\0fix\0null", 12);
    auto resp = HttpResponse::String(binary_data, HttpStatus::kOk, mime::kApplicationOctetStream);

    std::string wire = resp.FormatWireResponse(/*keep_alive=*/true);
    EXPECT_TRUE(absl::StrContains(wire, absl::StrCat(headers::kContentLength, ": 12\r\n")));
}

TEST(HttpMessageTest, HttpResponseEmptyFactoryAndCloseConnection) {
    auto resp = HttpResponse::Empty(HttpStatus::kNoContent)
                        .WithHeader(headers::kAccessControlAllowOrigin, "*");

    EXPECT_EQ(resp.Status(), HttpStatus::kNoContent);
    EXPECT_EQ(resp.StatusCode(), 204);
    EXPECT_TRUE(resp.Body().empty());

    std::string wire = resp.FormatWireResponse(/*keep_alive=*/false);
    EXPECT_TRUE(absl::StartsWith(wire, "HTTP/1.1 204 No Content\r\n"));
    EXPECT_FALSE(absl::StrContains(wire, "-length:"));
    EXPECT_FALSE(absl::StrContains(wire, "-Length:"));
    EXPECT_TRUE(absl::StrContains(wire, absl::StrCat(headers::kConnection, ": close\r\n")));
    EXPECT_TRUE(
            absl::StrContains(wire, absl::StrCat(headers::kAccessControlAllowOrigin, ": *\r\n")));
}

TEST(HttpMessageTest, HttpResponseErrorFormatting) {
    auto resp = HttpResponse::String("Not Found", 404);
    std::string wire = resp.FormatWireResponse(/*keep_alive=*/false);
    EXPECT_TRUE(absl::StartsWith(wire, "HTTP/1.1 404 Not Found\r\n"));
}

TEST(HttpMessageTest, HttpResponseHeadRequestOmitsBodyPreservesContentLength) {
    auto resp = HttpResponse::String("Full Payload Body", HttpStatus::kOk, mime::kTextPlain);
    std::string wire = resp.FormatWireResponse(/*keep_alive=*/true, /*is_head_request=*/true);

    EXPECT_TRUE(absl::StartsWith(wire, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::StrContains(
            wire, absl::StrCat(headers::kContentType, ": ", mime::kTextPlain, "\r\n")));
    EXPECT_TRUE(absl::StrContains(wire, absl::StrCat(headers::kContentLength, ": 17\r\n")));
    EXPECT_TRUE(absl::EndsWith(wire, "\r\n\r\n"));
    EXPECT_FALSE(absl::StrContains(wire, "Full Payload Body"));
}

TEST(HttpMessageTest, HttpResponseLargeHeadersAndBodyExactWireFormatting) {
    constexpr std::string_view kBody = "{\"status\":\"ok\",\"emulator\":\"goldfish\"}";
    auto resp = HttpResponse::String(std::string(kBody), HttpStatus::kOk, mime::kApplicationJson)
                        .WithHeader(headers::kAccessControlAllowOrigin, "http://localhost:8080")
                        .WithHeader(headers::kAccessControlAllowMethods,
                                    "GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH")
                        .WithHeader(headers::kAccessControlAllowHeaders,
                                    "content-type,x-grpc-web,x-user-agent,authorization")
                        .WithHeader(headers::kAccessControlExposeHeaders,
                                    "grpc-status,grpc-message,grpc-status-details-bin")
                        .WithHeader(headers::kCacheControl, "no-store, no-cache, must-revalidate")
                        .WithHeader(headers::kXGrpcWeb, "1");

    const std::string wire = resp.FormatWireResponse(/*keep_alive=*/true);
    EXPECT_TRUE(absl::StartsWith(wire, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(absl::StrContains(wire, "access-control-allow-origin: http://localhost:8080\r\n"));
    EXPECT_TRUE(absl::StrContains(
            wire,
            "access-control-allow-methods: GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH\r\n"));
    EXPECT_TRUE(absl::StrContains(wire,
                                  "access-control-allow-headers: "
                                  "content-type,x-grpc-web,x-user-agent,authorization\r\n"));
    EXPECT_TRUE(absl::StrContains(
            wire,
            "access-control-expose-headers: grpc-status,grpc-message,grpc-status-details-bin\r\n"));
    EXPECT_TRUE(absl::StrContains(wire, "cache-control: no-store, no-cache, must-revalidate\r\n"));
    EXPECT_TRUE(absl::StrContains(wire, "x-grpc-web: 1\r\n"));
    EXPECT_TRUE(absl::StrContains(wire, absl::StrCat("content-length: ", kBody.size(), "\r\n")));
    EXPECT_TRUE(absl::StrContains(wire, "connection: keep-alive\r\n"));
    EXPECT_TRUE(absl::EndsWith(wire, absl::StrCat("\r\n\r\n", kBody)));

    // Verify 304 Not Modified and 204 No Content with non-empty bodies omit body on wire.
    auto resp_304 = HttpResponse::String(std::string(kBody), HttpStatus::kNotModified);
    const std::string wire_304 = resp_304.FormatWireResponse(/*keep_alive=*/false);
    EXPECT_TRUE(absl::StartsWith(wire_304, "HTTP/1.1 304 Not Modified\r\n"));
    EXPECT_TRUE(
            absl::StrContains(wire_304, absl::StrCat("content-length: ", kBody.size(), "\r\n")));
    EXPECT_TRUE(absl::EndsWith(wire_304, "\r\n\r\n"));
    EXPECT_FALSE(absl::StrContains(wire_304, kBody));

    // Verify large body HEAD request does not reserve or append the payload body.
    auto large_head = HttpResponse::String(std::string(65536, 'x'), HttpStatus::kOk,
                                           mime::kApplicationOctetStream);
    const std::string wire_head =
            large_head.FormatWireResponse(/*keep_alive=*/true, /*is_head_request=*/true);
    EXPECT_TRUE(absl::StrContains(wire_head, "content-length: 65536\r\n"));
    EXPECT_TRUE(absl::EndsWith(wire_head, "\r\n\r\n"));
    EXPECT_LT(wire_head.capacity(), 512u);
}

TEST(HttpMessageTest, StatusCodeBoundaryChecks) {
    auto r100 = HttpResponse::Empty(100);
    EXPECT_EQ(r100.StatusCode(), 100);
    std::string w100 = r100.FormatWireResponse(true);
    EXPECT_TRUE(absl::StartsWith(w100, "HTTP/1.1 100 Continue\r\n"));
    EXPECT_FALSE(absl::StrContains(w100, "-length:"));
    EXPECT_FALSE(absl::StrContains(w100, "-Length:"));

    auto r599 = HttpResponse::Empty(599);
    EXPECT_EQ(r599.StatusCode(), 599);
    EXPECT_TRUE(
            absl::StartsWith(r599.FormatWireResponse(false), "HTTP/1.1 599 Unknown Status\r\n"));

    auto r418 = HttpResponse::String("I'm a teapot", 418);
    EXPECT_EQ(r418.StatusCode(), 418);
    EXPECT_TRUE(absl::StartsWith(r418.FormatWireResponse(false), "HTTP/1.1 418 I'm a teapot\r\n"));

    auto r999 = HttpResponse::Empty(999);
    EXPECT_EQ(r999.StatusCode(), 999);
    EXPECT_TRUE(
            absl::StartsWith(r999.FormatWireResponse(false), "HTTP/1.1 999 Unknown Status\r\n"));
}

TEST(HttpMessageTest, HeaderHygieneAndMultiValueMerging) {
    HttpRequest req;
    HttpRequestTestPeer::SetRequest(
            &req, HttpMethod::kGet, "/test",
            {{"accept", "text/html, application/json"}, {"x-custom-token", "tok_abc"}}, "", true);

    EXPECT_EQ(req.GetHeader("accept"), "text/html, application/json");
    EXPECT_EQ(req.GetHeader("ACCEPT"), "text/html, application/json");
    EXPECT_EQ(req.GetHeader("x-custom-token"), "tok_abc");
    EXPECT_EQ(req.GetHeader("X-Custom-Token"), "tok_abc");
}

TEST(HttpMessageTest, HeterogeneousHeaderLookup) {
    HttpRequest req;
    HttpRequestTestPeer::SetRequest(&req, HttpMethod::kGet, "/test",
                                    {{"content-type", "application/json"}, {"origin", "localhost"}},
                                    "", true);

    // Look up via std::string_view slice
    std::string_view ct_slice = "content-type";
    EXPECT_TRUE(req.HasHeader(ct_slice));
    EXPECT_EQ(req.GetHeader(ct_slice), "application/json");

    // Case-insensitive std::string_view slice
    std::string_view ct_upper = "Content-Type";
    EXPECT_TRUE(req.HasHeader(ct_upper));
    EXPECT_EQ(req.GetHeader(ct_upper), "application/json");
}

TEST(HttpMessageTest, LongHeaderNameChunkedCaseInsensitiveHashing) {
    const std::string lower_key =
            "x-custom-very-long-extension-header-name-exceeding-sixty-four-bytes-for-chunk-hash-"
            "testing";
    const std::string mixed_key =
            "X-Custom-Very-Long-Extension-Header-Name-Exceeding-Sixty-Four-Bytes-For-Chunk-Hash-"
            "Testing";
    ASSERT_GT(lower_key.size(), 64u);

    HttpRequest req;
    HttpRequestTestPeer::SetRequest(&req, HttpMethod::kGet, "/test",
                                    {{"a", "len1"},
                                     {"ab", "len2"},
                                     {"abc", "len3"},
                                     {"host", "len4"},
                                     {"range", "len5"},
                                     {"origin", "len6"},
                                     {"referer", "len7"},
                                     {"location", "len8"},
                                     {lower_key, "chunk_ok"}},
                                    "", true);

    EXPECT_EQ(req.GetHeader("A"), "len1");
    EXPECT_EQ(req.GetHeader("aB"), "len2");
    EXPECT_EQ(req.GetHeader("AbC"), "len3");
    EXPECT_EQ(req.GetHeader("HoSt"), "len4");
    EXPECT_EQ(req.GetHeader("RaNgE"), "len5");
    EXPECT_EQ(req.GetHeader("OrIgIn"), "len6");
    EXPECT_EQ(req.GetHeader("ReFeReR"), "len7");
    EXPECT_EQ(req.GetHeader("LoCaTiOn"), "len8");
    EXPECT_TRUE(req.HasHeader(lower_key));
    EXPECT_TRUE(req.HasHeader(mixed_key));
    EXPECT_EQ(req.GetHeader(mixed_key), "chunk_ok");

    // Verify ASCII boundary symbols ('@'=0x40, '['=0x5B) are not conflated with '`'=0x60 or
    // '{'=0x7B
    CaseInsensitiveStringHash hasher;
    CaseInsensitiveStringEq eq;
    EXPECT_FALSE(eq("a@b[c", "a`b{c"));
    EXPECT_TRUE(eq("Az09@[_", "aZ09@[_"));
    EXPECT_EQ(hasher("Az09@[_"), hasher("aZ09@[_"));
}

}  // namespace goldfish::http
