
// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS);
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goldfish/devices/cable/cable.h"

namespace goldfish::adb {

// Maximum number of characters or bytes logged per ADB packet payload.
inline constexpr size_t kMaxDataLogLength = 512;

template <size_t N>
constexpr uint32_t MakeAdbCommand(const char (&s)[N]) {
    static_assert(N == 5, "ADB command must be exactly 4 characters long.");

    return static_cast<uint32_t>(s[0]) | (static_cast<uint32_t>(s[1]) << 8) |
           (static_cast<uint32_t>(s[2]) << 16) | (static_cast<uint32_t>(s[3]) << 24);
}

constexpr uint32_t kAdbCnxn = MakeAdbCommand("CNXN");
constexpr uint32_t kAdbOpen = MakeAdbCommand("OPEN");
constexpr uint32_t kAdbClse = MakeAdbCommand("CLSE");
constexpr uint32_t kAdbWrte = MakeAdbCommand("WRTE");
constexpr uint32_t kAdbOkay = MakeAdbCommand("OKAY");
constexpr uint32_t kAdbAuth = MakeAdbCommand("AUTH");

// ADB Sync protocol commands (used in WRTE payloads)
constexpr uint32_t kAdbSyncStat = MakeAdbCommand("STAT");
constexpr uint32_t kAdbSyncList = MakeAdbCommand("LIST");
constexpr uint32_t kAdbSyncRecv = MakeAdbCommand("RECV");
constexpr uint32_t kAdbSyncSend = MakeAdbCommand("SEND");
constexpr uint32_t kAdbSyncDone = MakeAdbCommand("DONE");
constexpr uint32_t kAdbSyncFail = MakeAdbCommand("FAIL");
constexpr uint32_t kAdbSyncDent = MakeAdbCommand("DENT");
constexpr uint32_t kAdbSyncQuit = MakeAdbCommand("QUIT");

#pragma pack(push, 1)
struct AMessage {
    uint32_t command;     /* command identifier constant      */
    uint32_t arg0;        /* first argument                   */
    uint32_t arg1;        /* second argument                  */
    uint32_t data_length; /* length of payload (0 is allowed) */
    uint32_t data_check;  /* checksum of data payload         */
    uint32_t magic;       /* command ^ 0xffffffff             */
};

struct APacket {
    AMessage message;
    char data[];
};
#pragma pack(pop)

/**
 * @class AdbPacketCallback
 * @brief Interface for receiving intercepted ADB packets and stream status updates.
 *
 * Implementations of this interface can be registered with AdbMessageLogger to
 * receive notifications when a full ADB packet is parsed or when the stream
 * goes out of sync due to errors.
 */
class AdbPacketCallback {
  public:
    virtual ~AdbPacketCallback() = default;

    /**
     * @brief Called when a full ADB packet has been successfully parsed.
     *
     * @param message The parsed ADB message header.
     * @param data Pointer to the payload data (may be nullptr if data_length is 0).
     * @param to_guest True if the packet is flowing from host to guest, false otherwise.
     */
    virtual void OnPacket(const AMessage& message, const char* data, bool to_guest) = 0;

    /**
     * @brief Called when the ADB stream goes out of sync and logging is disabled.
     *
     * This indicates a potential error or protocol desynchronization.
     *
     * @param reason A string describing the reason for the desynchronization.
     */
    virtual void OnOutOfSync(const std::string& reason, bool to_guest) = 0;
};

/**
 * @class AdbMessageLogger
 * @brief Parses raw bytes into ADB packets and notifies a callback.
 *
 * This class maintains a buffer of raw bytes received from a stream and attempts
 * to parse them into ADB packets. When a full packet is parsed, it notifies the
 * registered AdbPacketCallback.
 */
class AdbMessageLogger {
  public:
    /**
     * @brief Constructs an AdbMessageLogger.
     *
     * @param prefix String prefix for log messages.
     * @param to_guest True if this logger tracks traffic towards the guest.
     * @param verbose Enable verbose logging.
     */
    AdbMessageLogger(std::string prefix, bool to_guest, bool verbose)
            : prefix_(std::move(prefix)), to_guest_(to_guest), verbose_(verbose) {
        snippet_accumulator_.reserve(kMaxDataLogLength);
    }
    ~AdbMessageLogger();

    /**
     * @brief Processes raw bytes received from the stream.
     *
     * @param data Pointer to the raw data.
     * @param size Size of the data in bytes.
     */
    void Observe(const void* data, size_t size);

    /**
     * @brief Sets the callback to receive parsed packets.
     *
     * @param callback Pointer to the callback implementation.
     */
    void SetCallback(AdbPacketCallback* callback) { callback_ = callback; }

  private:
    void ReportOutOfSync(std::string_view summary, const std::string& reason);
    void EmitPacket(const char* snippet_data, size_t snippet_size);

    const std::string prefix_;
    bool out_of_sync_{false};
    const bool to_guest_;
    const bool verbose_;
    AdbPacketCallback* callback_{nullptr};

    enum class ParseState {
        kExpectHeader,
        kExpectSnippet,
        kSkipPayload
    } parse_state_{ParseState::kExpectHeader};

    AMessage header_;
    char header_buf_[sizeof(AMessage)];
    size_t header_buf_size_{0};
    std::vector<char> snippet_accumulator_;
    size_t snippet_needed_{0};
    size_t skip_remaining_{0};
};

/**
 * @class AdbLogger
 * @brief High-level sniffer that tracks ADB traffic in both directions.
 *
 * Implements the IDataSniffer interface to intercept traffic and uses
 * AdbMessageLogger and AdbBreadcrumbTracker to record events.
 */
class AdbLogger : public goldfish::devices::cable::IDataSniffer {
  public:
    /**
     * @brief Constructs an AdbLogger.
     *
     * @param host_port The port number on the host side.
     * @param guest_port The port number on the guest side.
     * @param verbose Enable verbose logging.
     */
    AdbLogger(int host_port, int guest_port, bool verbose, bool capture_snippets = true);
    ~AdbLogger() = default;

    /**
     * @brief Intercepts data flowing towards the guest.
     */
    void ToSocket(const void* data, size_t data_size) override;

    /**
     * @brief Intercepts data flowing towards the host.
     */
    void ToPlug(const void* data, size_t data_size) override;

  private:
    AdbMessageLogger to_guest_;
    AdbMessageLogger to_host_;
    std::unique_ptr<AdbPacketCallback> tracker_;
};
}  // namespace goldfish::adb
