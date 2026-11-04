# Copyright 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""End-to-end test for Clownfish with C++ videobridge, grpc-web-proxy, and aquarium-ui."""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
import urllib.request

import capabilities_service_pb2 as cap_pb
import common_pb2
import grpc
import pytest
import webrtc_service_pb2 as rtc_pb
import webrtc_service_pb2_grpc as rtc_grpc
from clownfish import companions
from clownfish.server import ClownfishServer

# Extend default TCP port readiness timeout for ASan companion binaries.
companions._wait_for_tcp_port.__defaults__ = (30.0,)

_SAMPLE_SDP_OFFER = (
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1 2\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:9 IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:tOQd\r\n"
    "a=ice-pwd:k9dNCBLXQGDoiFsCPHTsHFTh\r\n"
    "a=ice-options:trickle\r\n"
    "a=fingerprint:sha-256 "
    "39:4A:09:1E:0E:33:32:85:51:03:49:81:38:8C:44:4C:2E:C4:C4:BD:1B:2E:9C:00:F7:F9:F8:2C:AA:"
    "1C:D8:97\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:9 IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:tOQd\r\n"
    "a=ice-pwd:k9dNCBLXQGDoiFsCPHTsHFTh\r\n"
    "a=ice-options:trickle\r\n"
    "a=fingerprint:sha-256 "
    "39:4A:09:1E:0E:33:32:85:51:03:49:81:38:8C:44:4C:2E:C4:C4:BD:1B:2E:9C:00:F7:F9:F8:2C:AA:"
    "1C:D8:97\r\n"
    "a=setup:actpass\r\n"
    "a=mid:1\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:96 VP8/90000\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:tOQd\r\n"
    "a=ice-pwd:k9dNCBLXQGDoiFsCPHTsHFTh\r\n"
    "a=ice-options:trickle\r\n"
    "a=fingerprint:sha-256 "
    "39:4A:09:1E:0E:33:32:85:51:03:49:81:38:8C:44:4C:2E:C4:C4:BD:1B:2E:9C:00:F7:F9:F8:2C:AA:"
    "1C:D8:97\r\n"
    "a=setup:actpass\r\n"
    "a=mid:2\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:262144\r\n"
)


def _encode_grpc_web_frame(payload: bytes) -> bytes:
    """Packs a protobuf payload into a 5-byte gRPC-Web data frame."""
    return struct.pack(">BI", 0x00, len(payload)) + payload


def _decode_grpc_web_first_message(body: bytes) -> bytes:
    """Unpacks the first data frame (flag 0x00) from a gRPC-Web response."""
    assert len(body) >= 5, f"gRPC-Web body too short: {len(body)}"
    flag, length = struct.unpack(">BI", body[:5])
    assert flag == 0x00, f"Expected data frame (0x00), got {flag:#x}: {body!r}"
    return body[5 : 5 + length]


class ClownfishCompanionEndToEndTest(unittest.TestCase):
    """Verifies Clownfish with real C++ videobridge, grpc-web-proxy, and aquarium-ui."""

    _tmp: tempfile.TemporaryDirectory[str]
    server: ClownfishServer
    channel: grpc.Channel
    rtc: rtc_grpc.RtcServiceStub

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls._tmp = tempfile.TemporaryDirectory()
        cls.server = ClownfishServer(
            preset="foldable",
            port=0,
            enable_discovery=True,
            discovery_dir=cls._tmp.name,
            fps=30.0,
            enable_webrtc=True,
            videobridge_port=0,
            webrtc_max_dim=360,
            enable_grpc_web=True,
            grpc_web_port=0,
            enable_aquarium=True,
            aquarium_port=0,
        )
        try:
            cls.server.start()
        except Exception:
            cls._tmp.cleanup()
            raise
        cls.channel = grpc.insecure_channel(cls.server.address)
        cls.rtc = rtc_grpc.RtcServiceStub(cls.channel)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.channel.close()
        cls.server.stop()
        cls._tmp.cleanup()
        super().tearDownClass()

    def test_webrtc_v2_real_videobridge_jsep_handshake(self) -> None:
        self.assertIsNotNone(self.server.videobridge_address)

        # Request WebRTC stream via Clownfish (proxied to C++ videobridge)
        rtc_res = self.rtc.RequestRtcStream(
            rtc_pb.RtcStreamRequest(
                requested_tracks=[
                    common_pb2.TRACK_TYPE_SCREEN_PRIMARY,
                    common_pb2.TRACK_TYPE_AUDIO_SPEAKER,
                ]
            ),
            timeout=5.0,
        )
        session_id = rtc_res.handle.session_id
        self.assertTrue(session_id)

        # Open JSEP stream and send a real WebRTC SDP Offer
        jsep_stream = self.rtc.ReceiveJsepMessageStream(rtc_res.handle, timeout=10.0)
        self.rtc.SendJsepMessage(
            rtc_pb.JsepMessage(
                handle=rtc_res.handle,
                sdp=rtc_pb.SessionDescription(
                    type=rtc_pb.SessionDescription.OFFER,
                    sdp=_SAMPLE_SDP_OFFER,
                ),
            ),
            timeout=5.0,
        )

        # Receive real WebRTC SDP Answer generated by C++ libwebrtc in videobridge
        received_answer = None
        for msg in jsep_stream:
            if msg.HasField("sdp") and msg.sdp.type == rtc_pb.SessionDescription.ANSWER:
                received_answer = msg.sdp.sdp
                break
        jsep_stream.cancel()

        self.assertIsNotNone(received_answer)
        assert received_answer is not None
        self.assertIn("v=0", received_answer)
        self.assertIn("m=audio", received_answer)
        self.assertIn("m=video", received_answer)
        self.assertIn("a=ice-ufrag:", received_answer)
        self.assertIn("a=fingerprint:sha-256", received_answer)

        # Close stream cleanly
        self.rtc.CloseRtcStream(
            rtc_pb.CloseRtcStreamRequest(handle=rtc_res.handle), timeout=5.0
        )

    def test_grpc_web_proxy_and_aquarium_ui_http_servers(self) -> None:
        self.assertIsNotNone(self.server.grpc_web_url)
        self.assertIsNotNone(self.server.aquarium_url)
        assert self.server.grpc_web_url is not None
        assert self.server.aquarium_url is not None

        # Verify Aquarium UI serves index.html on GET /
        with urllib.request.urlopen(
            f"{self.server.aquarium_url}/", timeout=5.0
        ) as resp:
            self.assertEqual(resp.status, 200)
            html = resp.read().decode("utf-8", errors="replace")
            self.assertIn("<html", html.lower())

        # Verify standalone grpc-web-proxy forwards v2 CapabilitiesService unary RPC
        req_proto = cap_pb.GetDeviceCapabilitiesRequest().SerializeToString()
        req = urllib.request.Request(
            f"{self.server.grpc_web_url}/android.emulation.v2.CapabilitiesService/GetDeviceCapabilities",
            data=_encode_grpc_web_frame(req_proto),
            headers={
                "Content-Type": "application/grpc-web+proto",
                "X-Grpc-Web": "1",
            },
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            self.assertEqual(resp.status, 200)
            body = resp.read()
        msg_bytes = _decode_grpc_web_first_message(body)
        dev_cap = cap_pb.DeviceCapabilities.FromString(msg_bytes)
        self.assertEqual(dev_cap.metadata.display_name, "Pixel Fold API 35")

        # Verify Aquarium UI's embedded GrpcWebServer also forwards v2 RtcService
        rtc_req_proto = rtc_pb.RtcStreamRequest(
            requested_tracks=[common_pb2.TRACK_TYPE_SCREEN_PRIMARY]
        ).SerializeToString()
        rtc_http_req = urllib.request.Request(
            f"{self.server.aquarium_url}/android.emulation.v2.RtcService/RequestRtcStream",
            data=_encode_grpc_web_frame(rtc_req_proto),
            headers={
                "Content-Type": "application/grpc-web+proto",
                "X-Grpc-Web": "1",
            },
            method="POST",
        )
        with urllib.request.urlopen(rtc_http_req, timeout=5.0) as resp:
            self.assertEqual(resp.status, 200)
            rtc_body = resp.read()
        rtc_msg_bytes = _decode_grpc_web_first_message(rtc_body)
        rtc_stream_res = rtc_pb.RtcStreamResponse.FromString(rtc_msg_bytes)
        self.assertTrue(rtc_stream_res.handle.session_id)
        self.rtc.CloseRtcStream(
            rtc_pb.CloseRtcStreamRequest(handle=rtc_stream_res.handle), timeout=5.0
        )


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
