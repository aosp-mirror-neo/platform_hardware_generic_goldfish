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
#include "legacy_console_bridge.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "android/base/system.h"
#include "android/emulation/control/absl_status_translate.h"
#include "android/goldfish/ini_file.h"
#include "android/status/status_macros.h"
#include "emulator_controller.grpc.pb.h"
#include "emulator_controller.pb.h"
#include "goldfish/discovery/emulator_advertisement.h"
#include "gsm_commands.h"
#include "screen_record_commands.h"
#include "snapshot_commands.h"
#include "telnet_auth.h"

namespace goldfish::parsing {

struct SensorValues {
    std::vector<float> data;
};

template <>
struct ArgExtractor<SensorValues> {
    static absl::StatusOr<SensorValues> Extract(ArgStream& args) {
        auto remaining = args.Remaining();
        while (!args.Empty()) {
            args.Next();
        }
        std::vector<std::string_view> tokens =
                absl::StrSplit(remaining, absl::ByAnyChar(" :,\t"), absl::SkipEmpty());

        if (tokens.empty()) {
            return absl::InvalidArgumentError(
                    "Usage: \"set <sensorname> <value-a>[:<value-b>[:<value-c>]]\"");
        }

        SensorValues result;
        for (auto token : tokens) {
            float v;
            if (!absl::SimpleAtof(token, &v)) {
                return absl::InvalidArgumentError(
                        "Usage: \"set <sensorname> <value-a>[:<value-b>[:<value-c>]]\"");
            }
            result.data.push_back(v);
        }
        return result;
    }
};

struct LinuxEvent {
    int type;
    int code;
    int value;
};

struct EventSequence {
    std::vector<LinuxEvent> events;
};

struct EventAlias {
    std::string_view name;
    int value;
};

constexpr EventAlias kEventTypes[] = {
        {"EV_SYN", 0}, {"EV_KEY", 1}, {"EV_REL", 2}, {"EV_ABS", 3}};

constexpr EventAlias kEventKeyCodes[] = {
        {"KEY_ESC", 1},    {"KEY_BACKSPACE", 14}, {"KEY_ENTER", 28}, {"KEY_SPACE", 57},
        {"KEY_HOME", 102}, {"KEY_POWER", 116},    {"KEY_BACK", 158}};

template <size_t count>
bool ParseEventIntOrAlias(std::string_view input, const EventAlias (&aliases)[count], int* out) {
    if (absl::SimpleAtoi(input, out)) {
        return true;
    }
    for (const auto& alias : aliases) {
        if (input == alias.name) {
            *out = alias.value;
            return true;
        }
    }
    return false;
}

template <>
struct ArgExtractor<EventSequence> {
    static absl::StatusOr<EventSequence> Extract(ArgStream& args) {
        if (args.Empty()) {
            return absl::InvalidArgumentError("Usage: event send <type>:<code>:<value> ...");
        }
        EventSequence seq;
        while (!args.Empty()) {
            std::string token = args.Next();
            if (token.empty()) break;
            std::vector<std::string> parts = absl::StrSplit(token, ':');
            if (parts.size() != 3) {
                return absl::InvalidArgumentError(
                        absl::StrFormat("invalid event value in '%s', must be an integer", token));
            }

            int type = 0;
            if (!ParseEventIntOrAlias(parts[0], kEventTypes, &type)) {
                return absl::InvalidArgumentError(
                        absl::StrFormat("invalid event type in '%s', try 'event list "
                                        "types' for valid values",
                                        token));
            }

            int code = 0;
            if (!ParseEventIntOrAlias(parts[1], kEventKeyCodes, &code)) {
                return absl::InvalidArgumentError(
                        absl::StrFormat("invalid event code in '%s', try 'event list "
                                        "codes <type>' for valid values",
                                        token));
            }

            int value = 0;
            if (!absl::SimpleAtoi(parts[2], &value)) {
                return absl::InvalidArgumentError(
                        absl::StrFormat("invalid event value in '%s', must be an integer", token));
            }

            seq.events.push_back({type, code, value});
        }
        return seq;
    }
};

}  // namespace goldfish::parsing

namespace goldfish::telnet {

using android::emulation::control::GrpcStatusToAbslStatus;
using android::goldfish::IniFile;

namespace {
absl::StatusOr<std::string> GetPlatformConfigProperty(LegacyConsoleBridge::ConsoleContext& ctx,
                                                      const std::string& key) {
    ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
    ASSIGN_OR_RETURN(auto context, ctx.NewContext());

    google::protobuf::Empty request;
    android::emulation::control::EmulatorStatus response;
    RETURN_IF_ERROR(GrpcStatusToAbslStatus(stub->getStatus(context.get(), request, &response)));

    auto map = response.platformconfig();
    if (auto it = map.find(key); it != map.end()) {
        return it->second;
    }

    LOG(WARNING) << key << " not found in platform config for port " << ctx.Port();
    return absl::InternalError(absl::StrCat(key, " not found in platform config"));
}

absl::StatusOr<android::emulation::control::VmRunState::RunState> GetVmRunState(
        LegacyConsoleBridge::ConsoleContext& ctx,
        android::emulation::control::EmulatorController::StubInterface* stub) {
    ASSIGN_OR_RETURN(auto context, ctx.NewContext());
    google::protobuf::Empty request;
    android::emulation::control::VmRunState response;
    RETURN_IF_ERROR(GrpcStatusToAbslStatus(stub->getVmState(context.get(), request, &response)));
    return response.state();
}

absl::Status SetVmRunState(LegacyConsoleBridge::ConsoleContext& ctx,
                           android::emulation::control::EmulatorController::StubInterface* stub,
                           android::emulation::control::VmRunState::RunState state) {
    ASSIGN_OR_RETURN(auto context, ctx.NewContext());
    android::emulation::control::VmRunState request;
    request.set_state(state);
    google::protobuf::Empty response;
    return GrpcStatusToAbslStatus(stub->setVmState(context.get(), request, &response));
}

absl::Status SetVmRunState(LegacyConsoleBridge::ConsoleContext& ctx,
                           android::emulation::control::VmRunState::RunState state) {
    ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
    return SetVmRunState(ctx, stub.get(), state);
}

absl::StatusOr<android::emulation::control::BatteryState> GetBatteryStateOrDefault(
        LegacyConsoleBridge::ConsoleContext& ctx,
        android::emulation::control::EmulatorController::StubInterface* stub) {
    ASSIGN_OR_RETURN(auto context, ctx.NewContext());
    google::protobuf::Empty request;
    android::emulation::control::BatteryState state;
    if (!stub->getBattery(context.get(), request, &state).ok()) {
        state.set_hasbattery(true);
        state.set_ispresent(true);
        state.set_charger(android::emulation::control::BatteryState::AC);
        state.set_chargelevel(100);
        state.set_health(android::emulation::control::BatteryState::GOOD);
        state.set_status(android::emulation::control::BatteryState::CHARGING);
    }
    return state;
}

template <typename EnumType>
struct BatteryEnumMapping {
    EnumType value;
    std::string_view command_name;
    const char* display_name;
};

constexpr BatteryEnumMapping<android::emulation::control::BatteryState::BatteryStatus>
        kBatteryStatuses[] = {
                {android::emulation::control::BatteryState::UNKNOWN, "unknown", "Unknown"},
                {android::emulation::control::BatteryState::CHARGING, "charging", "Charging"},
                {android::emulation::control::BatteryState::DISCHARGING, "discharging",
                 "Discharging"},
                {android::emulation::control::BatteryState::NOT_CHARGING, "not-charging",
                 "Not charging"},
                {android::emulation::control::BatteryState::FULL, "full", "Full"},
};

constexpr BatteryEnumMapping<android::emulation::control::BatteryState::BatteryHealth>
        kBatteryHealths[] = {
                {android::emulation::control::BatteryState::GOOD, "good", "Good"},
                {android::emulation::control::BatteryState::OVERHEATED, "overheat", "Overheat"},
                {android::emulation::control::BatteryState::DEAD, "dead", "Dead"},
                {android::emulation::control::BatteryState::OVERVOLTAGE, "overvoltage",
                 "Overvoltage"},
                {android::emulation::control::BatteryState::FAILED, "failure",
                 "Unspecified failure"},
};

template <typename EnumType, size_t count>
const char* BatteryEnumToString(const BatteryEnumMapping<EnumType> (&table)[count], EnumType value) {
    for (const auto& entry : table) {
        if (entry.value == value) {
            return entry.display_name;
        }
    }
    return "Unknown";
}

template <typename EnumType, size_t count>
absl::StatusOr<EnumType> ParseBatteryEnum(const BatteryEnumMapping<EnumType> (&table)[count],
                                          std::string_view input, std::string_view error_message) {
    for (const auto& entry : table) {
        if (absl::EqualsIgnoreCase(input, entry.command_name)) {
            return entry.value;
        }
    }
    return absl::InvalidArgumentError(error_message);
}
}  // namespace

LegacyConsoleBridge::LegacyConsoleBridge(int port, std::filesystem::path token_path,
                                         goldfish::metrics::MetricsReporter& metrics_reporter)
        : port_(port), token_path_(std::move(token_path)), metrics_reporter_(metrics_reporter) {
    if (auto status = TelnetAuth::LoadOrCreateToken(16, token_path_); !status.ok()) {
        LOG(WARNING) << "Failed to load or create telnet auth token at " << token_path_ << ": "
                     << status.status();
    }
    CommandRegistryBuilder builder(
            token_path_, [this](std::string_view top_level_name, std::string_view canonical_path) {
                RecordCommand(top_level_name, canonical_path);
            });

    auto unimplemented_no_args = [](ConsoleContext& /*ctx*/) {
        return absl::UnimplementedError("not implemented");
    };
    auto unimplemented_str_arg = [](ConsoleContext& /*ctx*/, const std::string& /*arg*/) {
        return absl::UnimplementedError("not implemented");
    };
    auto noop_str_arg = [](ConsoleContext& /*ctx*/, const std::string& /*arg*/) {
        return absl::OkStatus();
    };
    auto discover_current_emulator = [](ConsoleContext& ctx) {
        return ctx.DiscoverEmulatorWithProperties({{"port.serial", std::to_string(ctx.Port())}});
    };
    auto trigger_crash = [](ConsoleContext& ctx,
                            const char* message) -> absl::StatusOr<std::string> {
        ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
        ctx.TriggerCrash();
        return message;
    };

    // --- Safe Root Commands ---

    builder.On("ping" /* do_ping */, "check if the emulator is alive",
               [](ConsoleContext& ctx) -> absl::StatusOr<std::string> {
                   // We check the stub is connected and the service is ready.
                   RETURN_IF_ERROR(ctx.EmulatorControllerStub().status());
                   return "I am alive!";
               });
    builder.Command("ping", "").Safe();

    builder.On("auth" /* do_auth */, "user authentication for the emulator console",
               "use 'auth <auth_token>' to get extended console functionality\r\n",
               [token_path = token_path_](ConsoleContext& ctx,
                                          const std::string& token) -> absl::StatusOr<std::string> {
                   ASSIGN_OR_RETURN(auto expected_token, TelnetAuth::ReadToken(token_path));
                   if (!expected_token.SecureEquals(token)) {
                       return absl::InvalidArgumentError(absl::StrCat(
                               "authentication token does not match ", token_path.string()));
                   }
                   ctx.authenticated = true;
                   return "Android Console: type 'help' for a list of commands";
               });
    builder.Command("auth", "").Safe();

    builder.On("quit|exit" /* do_quit */, "quit control session",
               [](ConsoleContext& /*ctx*/) { return absl::AbortedError("quit"); });
    builder.Command("quit|exit", "").Safe();

    // --- AVD Commands ---
    auto avd = builder.Command("avd", "control virtual device execution");
    avd.On("stop" /* do_avd_stop */, "stop the virtual device",
           [](ConsoleContext& ctx) -> absl::Status {
               ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
               ASSIGN_OR_RETURN(auto state, GetVmRunState(ctx, stub.get()));
               if (state != android::emulation::control::VmRunState::RUNNING) {
                   return absl::FailedPreconditionError("virtual device already stopped");
               }
               return SetVmRunState(ctx, stub.get(), android::emulation::control::VmRunState::STOP);
           });
    avd.On("start" /* do_avd_start */, "start/restart the virtual device",
           [](ConsoleContext& ctx) -> absl::Status {
               ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
               ASSIGN_OR_RETURN(auto state, GetVmRunState(ctx, stub.get()));
               if (state == android::emulation::control::VmRunState::RUNNING) {
                   return absl::FailedPreconditionError("virtual device already running");
               }
               return SetVmRunState(ctx, stub.get(),
                                    android::emulation::control::VmRunState::START);
           });
    avd.On("status" /* do_avd_status */, "query virtual device status",
           [](ConsoleContext& ctx) -> absl::StatusOr<std::string> {
               ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
               ASSIGN_OR_RETURN(auto state, GetVmRunState(ctx, stub.get()));
               if (state == android::emulation::control::VmRunState::RUNNING) {
                   return "virtual device is running";
               }
               return "virtual device is stopped";
           });
    avd.On("heartbeat" /* do_avd_heartbeat */,
           "query the heart heartbeat number of the guest system",
           [](ConsoleContext& ctx) -> absl::StatusOr<std::string> {
               ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
               ASSIGN_OR_RETURN(auto context, ctx.NewContext());

               google::protobuf::Empty request;
               android::emulation::control::EmulatorStatus response;
               RETURN_IF_ERROR(
                       GrpcStatusToAbslStatus(stub->getStatus(context.get(), request, &response)));
               return absl::StrCat("heartbeat: ", response.heartbeat());
           });
    avd.On("rewindaudio" /* do_avd_rewind_audio */, "rewind the input audio to the beginning",
           unimplemented_no_args);
    avd.On("pause" /* do_avd_pause */, "pause the virtual device",
           [](ConsoleContext& ctx) -> absl::Status {
               return SetVmRunState(ctx, android::emulation::control::VmRunState::PAUSED);
           });
    avd.On("resume" /* do_avd_resume */, "resume the virtual device",
           [](ConsoleContext& ctx) -> absl::Status {
               return SetVmRunState(ctx, android::emulation::control::VmRunState::RUNNING);
           });
    avd.On("hostmicon" /* do_avd_hostmicon */, "activate the host audio input device",
           unimplemented_no_args);
    avd.On("hostmicoff" /* do_avd_hostmicoff */, "deactivate the host audio input device",
           unimplemented_no_args);
    avd.On("bugreport" /* do_avd_bugreport */, "generate bug report info.",
           unimplemented_no_args);
    avd.On("id" /* do_avd_id */, "query virtual device ID",
           [](ConsoleContext& ctx) { return GetPlatformConfigProperty(ctx, "avd.id"); });
    avd.On("windowtype" /* do_avd_windowtype */, "query virtual device headless or qtwindow",
           unimplemented_no_args);

    avd.On("path" /* do_avd_path */, "query AVD path",
           [](ConsoleContext& ctx) { return GetPlatformConfigProperty(ctx, "avd.content_path"); });

    avd.On("discoverypath" /* do_avd_discoverypath */, "query AVD discovery path",
           [discover_current_emulator](ConsoleContext& ctx) -> absl::StatusOr<std::string> {
               ASSIGN_OR_RETURN(auto discovery, discover_current_emulator(ctx));
               return discovery.discovery_file.string();
           });
    avd.On("snapshotspath" /* do_snapshotspath */, "query AVD snapshots path",
           [](ConsoleContext& ctx) -> absl::StatusOr<std::string> {
               ASSIGN_OR_RETURN(auto avd_path, GetPlatformConfigProperty(ctx, "avd.content_path"));
               return (std::filesystem::path(avd_path) / "snapshots").string();
           });
    avd.On("snapshotpath" /* do_snapshotpath */, "query path to a particular AVD snapshot",
           [](ConsoleContext& ctx, const std::string& name) -> absl::StatusOr<std::string> {
               if (name.empty()) {
                   return absl::InvalidArgumentError("Usage: 'avd snapshotpath <name>'");
               }
               ASSIGN_OR_RETURN(auto avd_path, GetPlatformConfigProperty(ctx, "avd.content_path"));
               return (std::filesystem::path(avd_path) / "snapshots" / name).string();
           });

    // name and grpc are safe sub-commands
    avd.On("name" /* do_avd_name */, "query virtual device name",
           [](ConsoleContext& ctx) { return GetPlatformConfigProperty(ctx, "avd.name"); });
    avd.Sub("name", "").Safe();

    avd.On("grpc" /* do_avd_grpc_port */, "query the grpc port",
           [discover_current_emulator](ConsoleContext& ctx) -> absl::StatusOr<std::string> {
               ASSIGN_OR_RETURN(auto discovery, discover_current_emulator(ctx));
               if (auto it = discovery.properties.find("grpc.port");
                   it != discovery.properties.end()) {
                   return it->second;
               }
               return absl::NotFoundError("No active gRPC service.");
           });
    avd.Sub("grpc", "").Safe();

    auto snapshot = avd.Sub("snapshot", "state snapshot commands");
    RegisterSnapshotCommands(snapshot);

    avd.Safe();

    // --- Automation Commands ---
    auto automation = builder.Command("automation", "manage emulator automation");
    automation.On("record" /* do_automation_record */, "start recording a macro.",
                  unimplemented_str_arg);
    automation.On("stop-record" /* do_automation_stop_recording */, "stop recording a macro.",
                  unimplemented_no_args);
    automation.On("play" /* do_automation_play */, "playback macro.", unimplemented_str_arg);
    automation.On("stop-play" /* do_automation_stop_playback */, "stop playing macro.",
                  unimplemented_no_args);

    // --- Event Commands ---
    auto event = builder.Command("event", "simulate hardware events");
    event.On("send" /* do_event_send */, "send a series of events to the kernel",
             [](ConsoleContext& ctx, goldfish::parsing::EventSequence seq) -> absl::Status {
                 ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
                 for (const auto& ev : seq.events) {
                     if (ev.type == 1 /* EV_KEY */) {
                         ASSIGN_OR_RETURN(auto context, ctx.NewContext());
                         android::emulation::control::KeyboardEvent request;
                         request.set_keycode(ev.code);
                         request.set_codetype(android::emulation::control::KeyboardEvent::Evdev);
                         request.set_eventtype(
                                 ev.value == 0
                                         ? android::emulation::control::KeyboardEvent::keyup
                                         : android::emulation::control::KeyboardEvent::keydown);
                         google::protobuf::Empty response;
                         RETURN_IF_ERROR(GrpcStatusToAbslStatus(
                                 stub->sendKey(context.get(), request, &response)));
                     }
                 }
                 return absl::OkStatus();
             });

    event.On("types" /* do_event_types */, "list all <type> aliases",
             [](ConsoleContext& /*ctx*/) -> std::string {
                 return "event <type> can be an integer or one of the following aliases\\r\\n"
                        "    EV_SYN    (4 code aliases)\\r\\n"
                        "    EV_KEY    (124 code aliases)\\r\\n"
                        "    EV_REL    (8 code aliases)\\r\\n"
                        "    EV_ABS    (15 code aliases)\\r\\n"
                        "    EV_MSC    (3 code aliases)\\r\\n"
                        "    EV_SW     (4 code aliases)\\r\\n"
                        "    EV_LED    (5 code aliases)\\r\\n"
                        "    EV_SND    (3 code aliases)\\r\\n"
                        "    EV_REP    (2 code aliases)\\r\\n"
                        "    EV_FF   \\r\\n"
                        "    EV_PWR  \\r\\n"
                        "    EV_FF_STATUS";
             });

    event.On(
            "codes" /* do_event_codes */, "list all <code> aliases for a given <type>",
            [](ConsoleContext& /*ctx*/,
               std::optional<std::string> type_opt) -> absl::StatusOr<std::string> {
                if (!type_opt.has_value() || type_opt->empty()) {
                    return absl::InvalidArgumentError("argument missing, try 'event codes <type>'");
                }
                const std::string& type = *type_opt;
                if (type == "EV_KEY" || type == "1") {
                    return "type 'EV_KEY' accepts the following <code> aliases:\\r\\n"
                           "    KEY_0       \\r\\n"
                           "    KEY_1       \\r\\n"
                           "    KEY_2       \\r\\n"
                           "    KEY_3       \\r\\n"
                           "    KEY_4       \\r\\n"
                           "    KEY_5       \\r\\n"
                           "    KEY_6       \\r\\n"
                           "    KEY_7       \\r\\n"
                           "    KEY_8       \\r\\n"
                           "    KEY_9       \\r\\n"
                           "    KEY_A       \\r\\n"
                           "    KEY_B       \\r\\n"
                           "    KEY_C       \\r\\n"
                           "    KEY_D       \\r\\n"
                           "    KEY_E       \\r\\n"
                           "    KEY_F       \r\n"
                           "    KEY_G       \\r\\n"
                           "    KEY_H       \\r\\n"
                           "    KEY_I       \\r\\n"
                           "    KEY_J       \\r\\n"
                           "    KEY_K       \\r\\n"
                           "    KEY_L       \\r\\n"
                           "    KEY_M       \\r\\n"
                           "    KEY_N       \\r\\n"
                           "    KEY_O       \\r\\n"
                           "    KEY_P       \\r\\n"
                           "    KEY_Q       \\r\\n"
                           "    KEY_R       \\r\\n"
                           "    KEY_S       \\r\\n"
                           "    KEY_T       \\r\\n"
                           "    KEY_U       \\r\\n"
                           "    KEY_V       \\r\\n"
                           "    KEY_W       \\r\\n"
                           "    KEY_X       \\r\\n"
                           "    KEY_Y       \\r\\n"
                           "    KEY_Z       \\r\\n"
                           "    KEY_ENTER   \\r\\n"
                           "    KEY_ESC     \\r\\n"
                           "    KEY_BACKSPACE\\r\\n"
                           "    KEY_TAB     \\r\\n"
                           "    KEY_SPACE   \\r\\n"
                           "    KEY_HOME    \\r\\n"
                           "    KEY_BACK    \\r\\n"
                           "    KEY_POWER   \\r\\n"
                           "    KEY_VOLUMEUP\\r\\n"
                           "    KEY_VOLUMEDOWN";
                } else if (type == "EV_SYN" || type == "0") {
                    return "type 'EV_SYN' accepts the following <code> aliases:\\r\\n"
                           "    SYN_REPORT  \\r\\n"
                           "    SYN_CONFIG  \\r\\n"
                           "    SYN_MT_REPORT\\r\\n"
                           "    SYN_DROPPED ";
                } else if (type == "EV_REL" || type == "2") {
                    return "type 'EV_REL' accepts the following <code> aliases:\\r\\n"
                           "    REL_X       \\r\\n"
                           "    REL_Y       \\r\\n"
                           "    REL_Z       \\r\\n"
                           "    REL_RX      \\r\\n"
                           "    REL_RY      \\r\\n"
                           "    REL_RZ      \\r\\n"
                           "    REL_WHEEL   \\r\\n"
                           "    REL_MISC    ";
                } else if (type == "EV_ABS" || type == "3") {
                    return "type 'EV_ABS' accepts the following <code> aliases:\\r\\n"
                           "    ABS_X       \\r\\n"
                           "    ABS_Y       \\r\\n"
                           "    ABS_Z       \\r\\n"
                           "    ABS_RX      \\r\\n"
                           "    ABS_RY      \\r\\n"
                           "    ABS_RZ      \\r\\n"
                           "    ABS_HAT0X   \\r\\n"
                           "    ABS_HAT0Y   ";
                } else if (type == "EV_FF" || type == "EV_PWR" || type == "EV_FF_STATUS") {
                    return "no code aliases defined for this type";
                }
                return absl::InvalidArgumentError(
                        "bad argument, see 'event types' for valid values");
            });

    event.On("text" /* do_event_text */, "simulate keystrokes from a given text",
             [](ConsoleContext& ctx, ArgStream& args) -> absl::Status {
                 if (args.Empty()) {
                     return absl::InvalidArgumentError(
                             "argument missing, try 'event text <message>'");
                 }
                 std::string text = std::string(args.Remaining());

                 ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
                 ASSIGN_OR_RETURN(auto context, ctx.NewContext());

                 android::emulation::control::KeyboardEvent request;
                 request.set_text(text);
                 google::protobuf::Empty response;

                 return GrpcStatusToAbslStatus(stub->sendKey(context.get(), request, &response));
             });

    event.On("mouse" /* do_event_mouse */, "simulate a mouse event",
             [](ConsoleContext& ctx, int x, int y, int device, int buttonstate) -> absl::Status {
                 ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
                 ASSIGN_OR_RETURN(auto context, ctx.NewContext());

                 android::emulation::control::MouseEvent request;
                 request.set_x(x);
                 request.set_y(y);
                 request.set_buttons(buttonstate);
                 request.set_display(0);
                 google::protobuf::Empty response;

                 return GrpcStatusToAbslStatus(stub->sendMouse(context.get(), request, &response));
             });
    // --- Geo Commands ---
    auto geo = builder.Command("geo", "Geo-location commands");
    geo.On("nmea" /* do_geo_nmea */, "send a GPS NMEA sentence",
           "'geo nema <sentence>' sends an NMEA 0183 sentence to the emulated "
           "device, as\r\n"
           "if it came from an emulated GPS modem. <sentence> must begin with "
           "'$GP'. Only\r\n"
           "'$GPGGA' and '$GPRMC' sentences are supported at the moment.\r\n",
           unimplemented_str_arg);
    geo.On("fix" /* do_geo_fix */, "send a simple GPS fix",
           "'geo fix <longitude> <latitude> [<altitude> [<satellites> "
           "[<velocity> [<heading>]]]]'\r\n"
           " allows you to send a simple GPS fix to the emulated system.\r\n"
           " The parameters are:\r\n\r\n"
           "  <longitude>   longitude, in decimal degrees\r\n"
           "  <latitude>    latitude, in decimal degrees\r\n"
           "  <altitude>    optional altitude in meters\r\n"
           "  <satellites>  number of satellites being tracked (1-12)\r\n"
           "  <velocity>    optional velocity in knots\r\n"
           "  <heading>     optional heading in degrees [0.0, 360.0]\r\n"
           "\r\n",
           [](ConsoleContext& ctx, double longitude, double latitude,
              std::optional<double> altitude, std::optional<int> satellites,
              std::optional<double> velocity, std::optional<double> heading) {
               // Input validation
               if (latitude < -90.0 || latitude > 90.0) {
                   return absl::InvalidArgumentError("Latitude must be between -90 and 90.");
               }
               if (longitude < -180.0 || longitude > 180.0) {
                   return absl::InvalidArgumentError("Longitude must be between -180 and 180.");
               }

               android::emulation::control::GpsState request;
               request.set_longitude(longitude);
               request.set_latitude(latitude);
               if (altitude) {
                   request.set_altitude(*altitude);
               }
               constexpr int kDefaultGeoFixSatellites = 4;
               const int num_satellites = satellites.value_or(kDefaultGeoFixSatellites);
               if (num_satellites < 0 || num_satellites > 12) {
                   return absl::InvalidArgumentError("Satellites must be between 0 and 12.");
               }
               request.set_satellites(num_satellites);
               if (velocity) {
                   // Convert knots to m/s
                   request.set_speed(*velocity * 0.514444);
               }
               if (heading) {
                   if (*heading < 0.0 || *heading > 360.0) {
                       return absl::InvalidArgumentError("Heading must be between 0 and 360.");
                   }
                   request.set_bearing(*heading);
               }
               ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
               ASSIGN_OR_RETURN(auto context, ctx.NewContext());
               google::protobuf::Empty unused;
               return GrpcStatusToAbslStatus(stub->setGps(context.get(), request, &unused));
           });
    geo.On("gnss" /* do_geo_gnss */, "send a GNSS sentence",
           "'geo gnss <sentence>' sends a GNSS sentence to the emulated "
           "device.\r\n"
           "<sentence> has fields separated by ',', and it starts with 8 "
           "fields\r\n"
           "for clock data, 1 field for measurement count, and followed by\r\n"
           "count * 9 (each measurement data has 9 fields).\r\n"
           "e.g. geo gnss "
           "TimeNanos,FullBiasNanos,BiasNanos,BiasUncertaintyNanos,\r\n"
           "DriftNanosPerSecond,DriftUncertaintyNanosPerSecond,"
           "HardwareClockDiscontinuityCount,\r\n",
           unimplemented_str_arg);

    // --- GSM Commands ---
    auto gsm = builder.Command("gsm", "GSM related commands");
    RegisterGsmCommands(gsm);

    // --- CDMA Commands ---
    auto cdma = builder.Command("cdma", "CDMA related commands");
    cdma.On("ssource" /* do_cdma_ssource */, "Set the current CDMA subscription source",
            [](ConsoleContext& /*ctx*/, const std::string& source) -> absl::Status {
                if (source != "nv" && source != "ruim") {
                    return absl::InvalidArgumentError(
                            absl::StrFormat("Don't know source %s", source));
                }
                return absl::OkStatus();
            });

    cdma.On("prl_version" /* do_cdma_prl_version */, "Dump the current PRL version",
            [](ConsoleContext& /*ctx*/, int /*version*/) -> absl::Status {
                return absl::OkStatus();
            });

    // --- Network Commands ---
    auto network = builder.Command("network", "manage network settings");
    network.On("status" /* do_network_status */, "dump network status",
               [](ConsoleContext& /*ctx*/) -> std::string {
                   return "Current network status:\\r\\n"
                          "  download speed:   10000000 bits/s (1220.7 KB/s)\\r\\n"
                          "  upload speed:     10000000 bits/s (1220.7 KB/s)\\r\\n"
                          "  minimum latency:  0 ms\\r\\n"
                          "  maximum latency:  0 ms";
               });

    network.On("speed" /* do_network_speed */, "change network speed", noop_str_arg);
    network.On("delay" /* do_network_delay */, "change network latency", noop_str_arg);

    auto capture = network.Sub("capture", "dump network packets to file");
    capture.On("start" /* do_network_capture_start */, "start network capture",
               [](ConsoleContext& /*ctx*/, const std::string& file) -> absl::StatusOr<std::string> {
                   return absl::StrFormat("capturing to %s", file);
               });

    capture.On("stop" /* do_network_capture_stop */, "stop network capture",
               [](ConsoleContext& /*ctx*/) -> absl::Status { return absl::OkStatus(); });

    // --- WiFi Commands ---
    auto wifi = builder.Command("wifi", "manage wifi settings");
    wifi.On("add" /* do_wifi_add */, "add new WiFi SSID",
            [](ConsoleContext& /*ctx*/, const std::string& /*ssid*/,
               std::optional<std::string> /*password*/) -> absl::Status {
                return absl::OkStatus();
            });

    wifi.On("block" /* do_wifi_block */, "block network access on SSID", noop_str_arg);
    wifi.On("unblock" /* do_wifi_unblock */, "unblock network access on SSID", noop_str_arg);

    // --- Power Commands ---
    auto update_battery =
            [](ConsoleContext& ctx,
               std::function<void(android::emulation::control::BatteryState&)> mutator)
            -> absl::Status {
        ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
        ASSIGN_OR_RETURN(auto state, GetBatteryStateOrDefault(ctx, stub.get()));

        mutator(state);

        ASSIGN_OR_RETURN(auto context_set, ctx.NewContext());
        google::protobuf::Empty empty_res;
        return GrpcStatusToAbslStatus(stub->setBattery(context_set.get(), state, &empty_res));
    };

    auto power = builder.Command("power", "power related commands");
    power.On(
            "display" /* do_power_display */, "display battery and charger state",
            [](ConsoleContext& ctx) -> absl::StatusOr<std::string> {
                ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
                ASSIGN_OR_RETURN(auto state, GetBatteryStateOrDefault(ctx, stub.get()));

                const char* ac_str =
                        (state.charger() == android::emulation::control::BatteryState::AC)
                                ? "online"
                                : "offline";
                const char* status_str = BatteryEnumToString(kBatteryStatuses, state.status());
                const char* health_str = BatteryEnumToString(kBatteryHealths, state.health());
                const char* present_str = state.ispresent() ? "true" : "false";

                return absl::StrFormat(
                        "AC: %s\\r\\nstatus: %s\\r\\nhealth: %s\\r\\npresent: %s\\r\\ncapacity: %d",
                        ac_str, status_str, health_str, present_str, state.chargelevel());
            });

    power.On("ac" /* do_ac_state */, "set AC charging state",
             [update_battery](ConsoleContext& ctx, const std::string& state) -> absl::Status {
                 if (!absl::EqualsIgnoreCase(state, "on") &&
                     !absl::EqualsIgnoreCase(state, "off")) {
                     return absl::InvalidArgumentError("Usage: \"ac on\" or \"ac off\"");
                 }
                 const auto charger = absl::EqualsIgnoreCase(state, "on")
                                              ? android::emulation::control::BatteryState::AC
                                              : android::emulation::control::BatteryState::NONE;
                 return update_battery(ctx, [charger](auto& b) { b.set_charger(charger); });
             });

    power.On("status" /* do_battery_status */, "set battery status",
             [update_battery](ConsoleContext& ctx, const std::string& state) -> absl::Status {
                 ASSIGN_OR_RETURN(
                         auto battery_status,
                         ParseBatteryEnum(kBatteryStatuses, state, "invalid battery status"));
                 return update_battery(
                         ctx, [battery_status](auto& b) { b.set_status(battery_status); });
             });

    power.On("present" /* do_battery_present */, "set battery present state",
             [update_battery](ConsoleContext& ctx, const std::string& state) -> absl::Status {
                 if (!absl::EqualsIgnoreCase(state, "true") &&
                     !absl::EqualsIgnoreCase(state, "false")) {
                     return absl::InvalidArgumentError("invalid argument, use true or false");
                 }
                 const bool present = absl::EqualsIgnoreCase(state, "true");
                 return update_battery(ctx, [present](auto& b) { b.set_ispresent(present); });
             });

    power.On("health" /* do_battery_health */, "set battery health state",
             [update_battery](ConsoleContext& ctx, const std::string& state) -> absl::Status {
                 ASSIGN_OR_RETURN(
                         auto battery_health,
                         ParseBatteryEnum(kBatteryHealths, state, "invalid battery health"));
                 return update_battery(
                         ctx, [battery_health](auto& b) { b.set_health(battery_health); });
             });

    power.On("capacity" /* do_battery_capacity */, "set battery capacity percentage",
             [update_battery](ConsoleContext& ctx, int capacity) -> absl::Status {
                 if (capacity < 0 || capacity > 100) {
                     return absl::InvalidArgumentError("invalid capacity percentage");
                 }
                 return update_battery(ctx, [capacity](auto& b) { b.set_chargelevel(capacity); });
             });

    // --- Redir Commands ---
    auto redir = builder.Command("redir", "manage port redirections");
    redir.On("list" /* do_redir_list */, "list current redirections",
             [](ConsoleContext& /*ctx*/) -> std::string { return "no active redirections"; });
    redir.On("add" /* do_redir_add_ipv4 */, "add new ipv4 redirection", noop_str_arg);
    redir.On("del" /* do_redir_del_ipv4 */, "remove existing ipv4 redirection", noop_str_arg);
    redir.On("add-ipv6" /* do_redir_add_ipv6 */, "add new ipv6 redirection", noop_str_arg);
    redir.On("del-ipv6" /* do_redir_del_ipv6 */, "remove existing ipv6 redirection", noop_str_arg);
    // --- SMS Commands ---
    auto sms = builder.Command("sms", "SMS related commands");
    sms.On("send" /* do_sms_send */, "send inbound SMS text message",
           [](ConsoleContext& /*ctx*/, ArgStream& args) -> absl::Status {
               if (args.Empty()) {
                   return absl::InvalidArgumentError(
                           "missing argument, try 'sms send <phonenumber> <text message>'");
               }
               std::string phonenumber = args.Next();
               if (phonenumber.empty() || args.Empty()) {
                   return absl::InvalidArgumentError(
                           "missing argument, try 'sms send <phonenumber> <text message>'");
               }
               return absl::UnimplementedError(kSmsDeprecatedMessage);
           });
    sms.On("pdu" /* do_sms_sendpdu */, "send inbound SMS PDU",
           [](ConsoleContext& /*ctx*/, std::optional<std::string> hex) -> absl::Status {
               if (!hex.has_value() || hex->empty()) {
                   return absl::InvalidArgumentError("missing argument, try 'sms pdu <hexstring>'");
               }
               return absl::UnimplementedError(kSmsDeprecatedMessage);
           });
    // --- Sensor Commands ---
    struct SensorDefinition {
        std::string name;
        std::vector<std::string> aliases;
        int sensor_id;
        size_t value_count;
    };

    static const std::vector<SensorDefinition> kSupportedSensors = {
        {"acceleration", {"accelerometer"}, 0, 3},
        {"gyroscope", {"gyro"}, 1, 3},
        {"magnetic-field", {"magnetic", "magnetometer"}, 2, 3},
        {"orientation", {}, 3, 3},
        {"temperature", {}, 4, 1},
        {"proximity", {}, 5, 1},
        {"light", {"lux"}, 6, 1},
        {"pressure", {"barometer"}, 7, 1},
        {"humidity", {}, 8, 1},
        {"magnetic-field-uncalibrated", {}, 9, 6},
        {"gyroscope-uncalibrated", {}, 10, 6},
        {"hinge-angle0", {}, 11, 1},
        {"hinge-angle1", {}, 12, 1},
        {"hinge-angle2", {}, 13, 1},
        {"heart-rate", {}, 14, 1},
        {"rgbc-light", {}, 15, 4},
        {"wrist-tilt", {}, 16, 1},
        {"acceleration-uncalibrated", {}, 17, 6},
    };

    auto find_sensor_or_error =
            [](std::string_view name) -> absl::StatusOr<const SensorDefinition*> {
        for (const auto& def : kSupportedSensors) {
            if (absl::EqualsIgnoreCase(def.name, name)) {
                return &def;
            }
            for (const auto& alias : def.aliases) {
                if (absl::EqualsIgnoreCase(alias, name)) {
                    return &def;
                }
            }
        }
        return absl::NotFoundError(absl::StrFormat(
                "unknown sensor name: %s, run 'sensor status' to get available sensors.", name));
    };

    auto make_sensor_request = [](int sensor_id) {
        android::emulation::control::SensorValue request;
        request.set_target(
                static_cast<android::emulation::control::SensorValue::SensorType>(sensor_id));
        return request;
    };

    auto sensor = builder.Command("sensor", "manage emulator sensors");
    sensor.On("status" /* do_sensors_status */, "list available sensors and their status",
              [make_sensor_request](ConsoleContext& ctx) -> absl::StatusOr<std::string> {
                  ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());

                  std::string out;
                  for (const auto& def : kSupportedSensors) {
                      ASSIGN_OR_RETURN(auto context, ctx.NewContext());
                      auto request = make_sensor_request(def.sensor_id);
                      android::emulation::control::SensorValue reply;
                      auto status = stub->getSensor(context.get(), request, &reply);
                      absl::StrAppendFormat(&out, "%s: %s.\r\n", def.name,
                                            status.ok() ? "enabled" : "disabled");
                  }
                  return out;
              });

    sensor.On("get" /* do_sensors_get */, "get sensor values",
              "'get <sensorname>' get the values of a given sensor.\r\n",
              [find_sensor_or_error, make_sensor_request](
                      ConsoleContext& ctx,
                      std::optional<std::string> sensor_name) -> absl::StatusOr<std::string> {
                  if (!sensor_name || sensor_name->empty()) {
                      return absl::InvalidArgumentError("Usage: \"get <sensorname>\"");
                  }
                  ASSIGN_OR_RETURN(const auto* def, find_sensor_or_error(*sensor_name));
                  ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
                  ASSIGN_OR_RETURN(auto context, ctx.NewContext());
                  auto request = make_sensor_request(def->sensor_id);
                  android::emulation::control::SensorValue reply;
                  RETURN_IF_ERROR(
                          GrpcStatusToAbslStatus(stub->getSensor(context.get(), request, &reply)));

                  std::string out = absl::StrCat(*sensor_name, " = ");
                  const auto& data = reply.value().data();
                  for (size_t i = 0; i < def->value_count; ++i) {
                      float val = i < static_cast<size_t>(data.size()) ? data[i] : 0.0f;
                      absl::StrAppendFormat(&out, "%g%s", val,
                                            (i == def->value_count - 1) ? "" : ":");
                  }
                  return out;
              });

    sensor.On(
            "set" /* do_sensors_set */, "set sensor values",
            "'set <sensorname> <value-a>[:<value-b>[:<value-c>[...]]]' set the values of a given "
            "sensor.\r\n",
            [find_sensor_or_error, make_sensor_request](
                    ConsoleContext& ctx, std::optional<std::string> sensor_name,
                    std::optional<goldfish::parsing::SensorValues> parsed_values) -> absl::Status {
                if (!sensor_name || sensor_name->empty() || !parsed_values ||
                    parsed_values->data.empty()) {
                    return absl::InvalidArgumentError(
                            "Usage: \"set <sensorname> <value-a>[:<value-b>[:<value-c>]]\"");
                }
                ASSIGN_OR_RETURN(const auto* def, find_sensor_or_error(*sensor_name));

                ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
                ASSIGN_OR_RETURN(auto context, ctx.NewContext());

                auto request = make_sensor_request(def->sensor_id);
                for (float v : parsed_values->data) {
                    request.mutable_value()->add_data(v);
                }

                google::protobuf::Empty response;
                return GrpcStatusToAbslStatus(stub->setSensor(context.get(), request, &response));
            });
    // --- Physics Commands ---
    auto physics = builder.Command("physics", "manage physical model");
    physics.On("record-gt" /* do_physics_record_ground_truth */,
               "start recording ground truth of the physical model's 6dof poses",
               unimplemented_str_arg);
    physics.On("stop" /* do_physics_stop */, "stop recording ground truth", unimplemented_no_args);

    // --- Finger Commands ---
    auto send_fingerprint = [](ConsoleContext& ctx, bool is_touching,
                               int touch_id = 0) -> absl::Status {
        ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
        ASSIGN_OR_RETURN(auto context, ctx.NewContext());
        android::emulation::control::Fingerprint request;
        request.set_istouching(is_touching);
        request.set_touchid(touch_id);
        google::protobuf::Empty unused;
        return GrpcStatusToAbslStatus(stub->sendFingerprint(context.get(), request, &unused));
    };

    auto finger = builder.Command("finger", "manage emulator finger print");
    finger.On("touch" /* do_fingerprint_touch */, "touch finger print sensor with <fingerid>",
              [send_fingerprint](ConsoleContext& ctx, int fingerid) {
                  return send_fingerprint(ctx, true, fingerid);
              });
    finger.On("remove" /* do_fingerprint_remove */, "remove finger from the fingerprint sensor",
              [send_fingerprint](ConsoleContext& ctx) { return send_fingerprint(ctx, false); });

    // --- Multi-display Commands ---
    auto update_secondary_displays =
            [](ConsoleContext& ctx, int id,
               const android::emulation::control::DisplayConfiguration* replacement)
            -> absl::Status {
        ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
        ASSIGN_OR_RETURN(auto context, ctx.NewContext());

        google::protobuf::Empty empty_req;
        android::emulation::control::DisplayConfigurations current_configs;
        RETURN_IF_ERROR(GrpcStatusToAbslStatus(
                stub->getDisplayConfigurations(context.get(), empty_req, &current_configs)));

        android::emulation::control::DisplayConfigurations new_configs;
        bool matched = false;
        for (const auto& disp : current_configs.displays()) {
            if (disp.display() == 0) {
                continue;  // Display 0 is primary
            }
            if (disp.display() == static_cast<uint32_t>(id)) {
                matched = true;
                if (replacement != nullptr) {
                    *new_configs.add_displays() = *replacement;
                }
            } else {
                *new_configs.add_displays() = disp;
            }
        }
        if (!matched) {
            if (replacement == nullptr) {
                return absl::InvalidArgumentError("invalid display id");
            }
            *new_configs.add_displays() = *replacement;
        }

        ASSIGN_OR_RETURN(auto set_context, ctx.NewContext());
        android::emulation::control::DisplayConfigurations reply;
        return GrpcStatusToAbslStatus(
                stub->setDisplayConfigurations(set_context.get(), new_configs, &reply));
    };

    auto multidisplay = builder.Command("multidisplay", "configure the multi-display");
    multidisplay.On(
            "add" /* do_multi_display_add */, "add new or modify existing display",
            [update_secondary_displays](ConsoleContext& ctx, int id, int width, int height, int dpi,
                                        int flag) -> absl::Status {
                if (id < 1 || id > 8) {
                    return absl::InvalidArgumentError(
                            "invalid display id; valid ids are from 1 to 8 inclusive");
                }
                android::emulation::control::DisplayConfiguration disp;
                disp.set_display(id);
                disp.set_width(width);
                disp.set_height(height);
                disp.set_dpi(dpi);
                disp.set_flags(flag);
                return update_secondary_displays(ctx, id, &disp);
            });

    multidisplay.On(
            "del" /* do_multi_display_del */, "remove existing display",
            [update_secondary_displays](ConsoleContext& ctx, int id) -> absl::Status {
                if (id < 1 || id > 10) {
                    return absl::InvalidArgumentError("invalid display id");
                }
                return update_secondary_displays(ctx, id, nullptr);
            });
    // --- Proxy Commands ---
    auto proxy = builder.Command("proxy", "manage network proxy server settings");
    proxy.On("set" /* do_proxy_set */, "set the proxy server to use", unimplemented_str_arg);
    proxy.On("clear" /* do_proxy_clear */, "clear proxy server setting", unimplemented_no_args);

    // --- Other Terminal Root Commands ---
    builder.On("crash" /* do_crash */, "crash the emulator instance",
               [trigger_crash](ConsoleContext& ctx) {
                   return trigger_crash(ctx, "crashing emulator, bye bye");
               });
    builder.On("crash-on-exit" /* do_crash_on_exit */,
               "simulate crash on exit for the emulator instance",
               [trigger_crash](ConsoleContext& ctx) {
                   return trigger_crash(ctx, "crashing emulator on exit, bye bye");
               });
    builder.On("kill" /* do_kill */, "kill the emulator instance",
               [](ConsoleContext& ctx) -> absl::Status {
                   return SetVmRunState(ctx, android::emulation::control::VmRunState::SHUTDOWN);
               });
    builder.On(
            "restart" /* do_restart */, "restart the emulator",
            [](ConsoleContext& ctx) -> absl::StatusOr<std::string> {
                RETURN_IF_ERROR(SetVmRunState(ctx, android::emulation::control::VmRunState::RESET));
                return "restarting emulator, bye bye";
            });
    builder.Command("grpc", "enable the grpc endpoint")
            .On("start" /* do_start_grpc */, "start the grpc endpoint",
                [](ConsoleContext& ctx, int port) -> absl::StatusOr<std::string> {
                    if (port < 0 || port >= 65536) {
                        return absl::InvalidArgumentError("Usage: \"grpc <port>\"");
                    }
                    int active_port = ctx.Port();
                    if (port != 0 && active_port != 0 && active_port != port) {
                        return absl::StrFormat("Port has already been activated at port: %d",
                                               active_port);
                    }
                    return absl::StrFormat("gRPC endpoint available at port %d",
                                           active_port != 0 ? active_port : port);
                });

    builder.On("debug" /* do_debug */, "control the emulator debug output tags",
               [](ConsoleContext& /*ctx*/, const std::optional<std::string>& tags) -> absl::Status {
                   if (tags.has_value() && !tags->empty()) {
                       LOG(WARNING) << "Debug tags are deprecated and have no effect: " << *tags;
                   } else {
                       LOG(WARNING) << "Debug tags are deprecated and have no effect.";
                   }
                   return absl::InvalidArgumentError(
                           "warning: debug tags are deprecated and have no effect");
               });

    builder.On("rotate" /* do_rotate_90_clockwise */, "rotate the screen clockwise by 90 degrees",
               [](ConsoleContext& ctx) -> absl::Status {
                   ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
                   ASSIGN_OR_RETURN(auto context_get, ctx.NewContext());

                   android::emulation::control::PhysicalModelValue get_req;
                   get_req.set_target(android::emulation::control::PhysicalModelValue::ROTATION);
                   android::emulation::control::PhysicalModelValue current_state;
                   auto status = stub->getPhysicalModel(context_get.get(), get_req, &current_state);
                   if (!status.ok()) {
                       current_state.set_target(
                               android::emulation::control::PhysicalModelValue::ROTATION);
                       current_state.mutable_value()->add_data(0.0f);
                       current_state.mutable_value()->add_data(0.0f);
                       current_state.mutable_value()->add_data(0.0f);
                   }

                   float current_z = current_state.value().data_size() > 2
                                             ? current_state.value().data(2)
                                             : 0.0f;
                   float new_z = std::fmod(current_z - 90.0f, 360.0f);

                   ASSIGN_OR_RETURN(auto context_set, ctx.NewContext());
                   android::emulation::control::PhysicalModelValue request;
                   request.set_target(android::emulation::control::PhysicalModelValue::ROTATION);
                   request.mutable_value()->add_data(0.0f);
                   request.mutable_value()->add_data(0.0f);
                   request.mutable_value()->add_data(new_z);

                   google::protobuf::Empty response;
                   return GrpcStatusToAbslStatus(
                           stub->setPhysicalModel(context_set.get(), request, &response));
               });

    auto set_posture = [](ConsoleContext& ctx, float posture_val) -> absl::Status {
        ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
        ASSIGN_OR_RETURN(auto context, ctx.NewContext());

        android::emulation::control::PhysicalModelValue request;
        request.set_target(android::emulation::control::PhysicalModelValue::POSTURE);
        request.mutable_value()->add_data(posture_val);
        google::protobuf::Empty response;

        return GrpcStatusToAbslStatus(stub->setPhysicalModel(context.get(), request, &response));
    };

    builder.On("fold" /* do_fold */, "fold the device",
               [set_posture](ConsoleContext& ctx) { return set_posture(ctx, 1.0F); });
    builder.On("unfold" /* do_unfold */, "unfold the device",
               [set_posture](ConsoleContext& ctx) { return set_posture(ctx, 3.0F); });

    builder.On("posture" /* do_set_posture */, "set the device posture",
               "Usage: posture <posture_id>\r\n"
               "  1: closed\t2: half-opened\t3: opened\t4: flipped\t5: tent\r\n",
               [set_posture](ConsoleContext& ctx, std::optional<int> posture_val) -> absl::Status {
                   if (!posture_val.has_value() || *posture_val < 1 || *posture_val > 5) {
                       return absl::InvalidArgumentError(
                               "Usage: \"posture <posture_id>\" "
                               "1: closed\t2: half-opened\t3: opened\t4: flipped\t5: tent");
                   }
                   return set_posture(ctx, static_cast<float>(*posture_val));
               });
    builder.Command("icebox", "auto-snapshot on uncaught exceptions")
            .On("track" /* do_icebox_track */,
                "(experimental) track exceptions in <pid> and take (up to [max_snapshots]) "
                "snapshots when there are assert failures",
                [](ConsoleContext& /*ctx*/, int /*pid*/, std::optional<int> /*max_snapshots*/) {
                    return absl::UnimplementedError("not implemented");
                });

    builder.On("nodraw" /* do_no_draw */, "turn on/off NoDraw mode. (experimental)",
               unimplemented_str_arg);

    builder.On("resize-display" /* do_resize_display */, "resize display 0 resolution",
               "Usage: resize-display <index>.\n"
               "index: 0: original, 1: 720p, 2: 1080p, 3: 4k, 4: 1080x1080\n",
               [](ConsoleContext& ctx, std::optional<int> new_size) -> absl::Status {
                   if (!new_size.has_value()) {
                       return absl::InvalidArgumentError("usage: \"resize-display <index>\"");
                   }
                   if (*new_size < 0 || *new_size > 2) {
                       return absl::InvalidArgumentError(absl::StrFormat(
                               "resize-display: size index %d not supported", *new_size));
                   }

                   ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
                   ASSIGN_OR_RETURN(auto context, ctx.NewContext());

                   android::emulation::control::DisplayMode request;
                   request.set_value(
                           static_cast<android::emulation::control::DisplayModeValue>(*new_size));
                   google::protobuf::Empty response;

                   return GrpcStatusToAbslStatus(
                           stub->setDisplayMode(context.get(), request, &response));
               });
    builder.On("virtualscene-image" /* do_set_virtualscene_image */,
               "customize virtualscene image for virtulscene camera",
               "Usage: virtualscene-image <wall|table> [path-to-image].\n"
               "If path-to-image is void, restore default image.\n",
               [](ConsoleContext& /*ctx*/, const std::string& /*target*/,
                  const std::optional<std::string>& /*path*/) {
                   return absl::UnimplementedError("not implemented");
               });

    builder.On("phonenumber" /* do_set_phone_number */, "set phone number for the device",
               "Usage: phonenumber <gsm-formatted number>.\n",
               [](ConsoleContext& /*ctx*/,
                  std::optional<std::string> phonenumber_opt) -> absl::Status {
                   if (!phonenumber_opt.has_value() || phonenumber_opt->empty()) {
                       return absl::InvalidArgumentError(
                               "usage: \"phonenumber <gsm-formatted number>\"");
                   }
                   const std::string& phonenumber = *phonenumber_opt;
                   for (char c : phonenumber) {
                       if (!std::isdigit(static_cast<unsigned char>(c)) && c != '+' && c != '#') {
                           return absl::InvalidArgumentError(
                                   absl::StrFormat("Failed to set phone number: %s", phonenumber));
                       }
                   }
                   return absl::OkStatus();
               });
    auto screenrecord = builder.Command("screenrecord", "Records the emulator's display");
    RegisterScreenRecordCommands(screenrecord);

    // Monitor is a root group in console.cpp but it's under qemu group usually
    builder.Command("qemu", "QEMU-specific commands")
            .On("monitor" /* do_qemu_monitor */, "enter QEMU monitor", unimplemented_no_args);

    registry_ = builder.Build();
}

LegacyConsoleBridge::~LegacyConsoleBridge() {
    ReportMetrics();
}

absl::StatusOr<std::string> LegacyConsoleBridge::operator()(std::string line, Context& ctx) {
    return (*registry_)(std::move(line), ctx);
}

std::string LegacyConsoleBridge::WelcomeMessage(const Context& ctx) const {
    return registry_->WelcomeMessage(ctx);
}

std::unique_ptr<LineCommandHandler::Context> LegacyConsoleBridge::CreateContext() const {
    auto ctx = std::make_unique<ConsoleContext>(port_);
    if (TelnetAuth::GetStatus(token_path_) == AuthStatus::kDisabled) {
        ctx->authenticated = true;
    }
    return ctx;
}

void LegacyConsoleBridge::RecordCommand(std::string_view top_level_name,
                                        std::string_view canonical_path) {
    // Set of commands we care about tracking.
    // Includes top-level commands and detailed subcommands for 'avd' and 'event'.
    static const absl::NoDestructor<absl::flat_hash_set<std::string_view>> kTrackedCommands({
        "help",
        "help-verbose",
        "ping",
        "automation",
        "geo",
        "gsm",
        "cdma",
        "crash",
        "crash-on-exit",
        "kill",
        "restart",
        "network",
        "grpc",
        "power",
        "quit",
        "redir",
        "sms",
        "qemu",
        "sensor",
        "physics",
        "finger",
        "debug",
        "rotate",
        "screenrecord",
        "fold",
        "unfold",
        "posture",
        "multidisplay",
        "icebox",
        "nodraw",
        "resize-display",
        "virtualscene-image",
        "proxy",
        "phonenumber",
        "auth",
        "avd stop",
        "avd start",
        "avd status",
        "avd heartbeat",
        "avd rewindaudio",
        "avd name",
        "avd grpc",
        "avd pause",
        "avd hostmicon",
        "avd hostmicoff",
        "avd resume",
        "avd bugreport",
        "avd id",
        "avd windowtype",
        "avd path",
        "avd discoverypath",
        "avd snapshotspath",
        "avd snapshotpath",
        "avd snapshot list",
        "avd snapshot save",
        "avd snapshot load",
        "avd snapshot del",
        "avd snapshot remap",
        "avd snapshot pull",
        "avd snapshot push",
        "avd snapshot last_loaded",
        "event send",
        "event types",
        "event codes",
        "event text",
        "event mouse",
    });

    auto it = kTrackedCommands->find(canonical_path);
    if (it == kTrackedCommands->end()) {
        it = kTrackedCommands->find(top_level_name);
        if (it == kTrackedCommands->end()) {
            return;
        }
    }

    const absl::MutexLock lock(metrics_mutex_);
    command_counts_[*it]++;
}

void LegacyConsoleBridge::ReportMetrics() {
    absl::flat_hash_map<std::string_view, int64_t> counts;
    {
        const absl::MutexLock lock(metrics_mutex_);
        if (command_counts_.empty()) {
            return;
        }
        counts = std::move(command_counts_);
        command_counts_.clear();
    }

    metrics_reporter_.Report(
            [counts = std::move(counts)](android_studio::AndroidStudioEvent& event) {
                event.set_kind(android_studio::AndroidStudioEvent::EMULATOR_CONSOLE_EVENTS);
                for (const auto& [command, count] : counts) {
                    if (count > 0) {
                        auto* console_event = event.add_emulator_console_events();
                        console_event->set_command(command);
                        console_event->set_source(android_studio::EmulatorConsoleEvent::TELNET);
                        console_event->set_value(count);
                    }
                }
            });
}

}  // namespace goldfish::telnet
