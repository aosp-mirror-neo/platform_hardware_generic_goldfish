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

#include "android/emulation/control/in_process_rtc_service.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/time/time.h"

#include "android/emulation/control/in_process_media_provider.h"
#include "emulator/plugin/audio/test_bql.h"
#include "emulator/plugin/audio/test_qemu_audio_stub.h"
#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "emulator/plugin/grpc/input/recording_input_dispatcher.h"
#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/audio/qemu_audio_capture.h"
#include "goldfish/avd/avd_info.h"
#include "goldfish/avd/avd_properties.h"
#include "goldfish/avd/avd_universe.h"
#include "goldfish/avd/global_event_loop.h"
#include "goldfish/display/test/fake_multi_display.h"
#include "goldfish/display/test/fake_pixman_display.h"
#include "input_service.pb.h"

extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming)
AudioBackend* audio_be_by_name(const char* name, void* errp);
struct QemuConsole {
    int dummy;
};
// NOLINTNEXTLINE(readability-identifier-naming)
QemuConsole* qemu_console_lookup_by_index(int index) {
    return nullptr;
}
}

namespace android::emulation::control {
namespace {

using ::goldfish::grpc::v2::IInputDispatcher;
using ::goldfish::grpc::v2::testing::RecordingInputDispatcher;

class TestAvdUniverse : public ::goldfish::avd::AvdUniverse {
  public:
    explicit TestAvdUniverse(::goldfish::display::IMultiDisplay* multi_display,
                             ::goldfish::async::EventLoop& loop)
            : ::goldfish::avd::AvdUniverse(std::make_unique<::goldfish::avd::AvdProperties>())
            , multi_display_(multi_display)
            , loop_(loop) {
        ::goldfish::avd::SetGlobalAvdUniverse(this);
    }

    ~TestAvdUniverse() override { ::goldfish::avd::SetGlobalAvdUniverse(nullptr); }

    ::goldfish::async::EventLoop& GetQemuEventLoop() override { return loop_; }
    ::goldfish::metrics::MetricsReporter& GetMetricsReporter() override {
        LOG(FATAL) << "GetMetricsReporter not implemented in tests";
    }
    ::android::goldfish::VmOperations& GetVmOperations() override {
        LOG(FATAL) << "GetVmOperations not implemented in tests";
    }
    ::goldfish::display::IMultiDisplay& GetMultiDisplay() const override { return *multi_display_; }
    std::shared_ptr<IInputDispatcher> GetInputDispatcher() override { return input_dispatcher_; }

    absl::Status OnSave(::goldfish::archive::IWriter&) const override { return absl::OkStatus(); }
    absl::Status OnLoad(::goldfish::archive::IReader&) override { return absl::OkStatus(); }

  private:
    ::goldfish::display::IMultiDisplay* multi_display_;
    ::goldfish::async::EventLoop& loop_;
    std::shared_ptr<RecordingInputDispatcher> input_dispatcher_ =
            std::make_shared<RecordingInputDispatcher>();
};

#undef send
class FakeKeyEventSender : public keyboard::IKeyEventSender {
  public:
    void send(const KeyboardEvent request) override { sent_events.push_back(request); }

    std::vector<KeyboardEvent> sent_events;
};

TEST(InProcessRtcServiceTest, CreatesServicesFromAvdUniverse) {
    ::goldfish::display::test::FakeMultiDisplay fake_multidisplay(
            ::goldfish::avd::globalEventLoop());
    TestAvdUniverse universe(&fake_multidisplay, *::goldfish::avd::globalEventLoop());

    auto services = CreateInProcessRtcServices(universe, 0, 0);
    EXPECT_EQ(services.size(), 2);
    EXPECT_NE(services[0], nullptr);
    EXPECT_NE(services[1], nullptr);
}

// Stands in for the WebRTC engine: plays a constant into whoever pulls playout.
class ConstantTransport : public ::webrtc::AudioTransport {
  public:
    explicit ConstantTransport(int16_t value) : value_(value) {}

    int32_t RecordedDataIsAvailable(const void*, size_t, size_t, size_t, uint32_t, uint32_t,
                                    int32_t, uint32_t, bool, uint32_t&) override {
        return 0;
    }
    int32_t NeedMorePlayData(size_t samples, size_t /*bytes_per_sample*/, size_t channels,
                             uint32_t /*rate_hz*/, void* audio, size_t& samples_out,
                             int64_t* elapsed, int64_t* ntp) override {
        ++calls;
        samples_out = samples * channels;
        std::fill_n(static_cast<int16_t*>(audio), samples_out, value_);
        *elapsed = 0;
        *ntp = 0;
        return 0;
    }
    void PullRenderData(int, int, size_t, size_t, void*, int64_t*, int64_t*) override {}

    int calls = 0;

  private:
    const int16_t value_;
};

class SilentHostInput final : public ::goldfish::audio::HostAudioInputInterface {
  public:
    HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* /*name*/,
                                  const ::goldfish::audio::GuestAudioConfig& /*config*/,
                                  GuestDataCallback /*on_host_data*/) override {
        return existing ? existing : &voice_;
    }
    void CloseHostVoice(HostVoiceHandle /*voice*/) override {}
    void SetHostVoiceActive(HostVoiceHandle /*voice*/, bool /*active*/) override {}
    void SetHostVoiceVolume(HostVoiceHandle /*voice*/,
                            const ::goldfish::audio::GuestChannelVolume& /*volume*/) override {}
    size_t ReadHostVoice(HostVoiceHandle /*voice*/, void* dst, size_t byte_count) override {
        std::memset(dst, 0, byte_count);
        return byte_count;
    }
    void SetTimerArmed(bool /*armed*/) override {}
    absl::Duration NowVirtualTime() const override { return absl::Milliseconds(now_ms); }

    int64_t now_ms = 1000;

  private:
    int voice_ = 1;
};

// When WebRTC starts playout on the audio device module the Switchboard plays received audio
// into, the guest microphone gets a "webrtc" input that pulls from that module.
TEST(InProcessRtcServiceTest, PlayoutReachesTheGuestMicrophoneThroughTheSharedAudioDevice) {
    using ::goldfish::audio::AudioInputMux;
    using ::goldfish::audio::MicrophoneInputKind;
    ::goldfish::audio::TestBql bql;
    ::android::goldfish::ScopedVmLock vm_lock;
    test_reset_audio_stubs();
    AudioBackend* backend = audio_be_by_name("mainaudiodev", nullptr);
    SilentHostInput host;
    auto mux = std::make_shared<AudioInputMux>(&host);
    ::goldfish::audio::RegisterAudioInputMux(backend, mux);
    // Mute the host mic so only the WebRTC input can produce non-silence.
    ASSERT_TRUE(mux->UpdateState({.input_updates = {{.source_id = "host", .muted = true}}}).ok());
    auto loop = ::goldfish::async::testing::TestEventLoop::Create();
    ::goldfish::display::test::FakeMultiDisplay fake_multidisplay(
            ::goldfish::avd::globalEventLoop());
    TestAvdUniverse universe(&fake_multidisplay, *loop);
    auto audio_device = ::goldfish::videobridge::GoldfishAudioDeviceModule::Create();
    ConstantTransport engine(1234);
    audio_device->RegisterAudioCallback(&engine);
    auto run_loop = [&loop] {
        ::android::goldfish::ScopedVmUnlock unlock;  // The loop thread takes the VM lock itself.
        loop->RunAll();
    };

    auto provider = CreateInProcessMediaProvider(universe, 0, backend, audio_device);
    audio_device->StartPlayout();
    run_loop();

    const auto inputs = mux->GetStateSnapshot().microphone_inputs;
    ASSERT_EQ(inputs.size(), 2U);
    EXPECT_EQ(inputs[1].source_id, "webrtc");
    EXPECT_EQ(inputs[1].kind, MicrophoneInputKind::kWebRtc);

    std::vector<int16_t> captured;
    ::goldfish::audio::MuxVoiceHandle* voice = nullptr;
    voice = mux->OpenVoice(nullptr, "virtio-sound.in",
                           {.sample_rate_hz = 48000,
                            .channels = 1,
                            .format = ::goldfish::audio::GuestPcmFormat::kS16},
                           [&](int avail_bytes) {
                               std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / 2);
                               const size_t read =
                                       mux->ReadVoice(voice, buf.data(), buf.size() * 2);
                               captured.insert(captured.end(), buf.begin(), buf.begin() + read / 2);
                           });
    mux->SetVoiceActive(voice, true);
    host.now_ms += 10;
    mux->OnTimerTick();
    EXPECT_THAT(captured, ::testing::AllOf(::testing::SizeIs(480), ::testing::Each(1234)));
    EXPECT_GT(engine.calls, 0);

    audio_device->StopPlayout();
    run_loop();
    EXPECT_EQ(mux->GetStateSnapshot().microphone_inputs.size(), 1U);

    // The module outlives the provider, as it may in the Switchboard's factory.
    provider.reset();
    audio_device->StartPlayout();
    EXPECT_EQ(loop->TaskCount(), 0U);

    mux->CloseVoice(voice);
    audio_device->RegisterAudioCallback(nullptr);
    ::goldfish::audio::UnregisterAudioInputMux(backend);
    mux->Shutdown();
    test_reset_audio_stubs();
}

TEST(InProcessRtcServiceTest, InputSenderDispatchesKeyEvent) {
    ::goldfish::display::test::FakeMultiDisplay fake_multidisplay(
            ::goldfish::avd::globalEventLoop());
    TestAvdUniverse universe(&fake_multidisplay, *::goldfish::avd::globalEventLoop());
    auto key_event_sender = std::make_shared<FakeKeyEventSender>();
    auto factory = CreateInProcessInputSenderFactory(fake_multidisplay, key_event_sender);
    auto sender = factory(DataChannelLabel::kInput);
    ASSERT_NE(sender, nullptr);
    EXPECT_TRUE(sender->Start().ok());

    InputEvent event;
    event.mutable_key_event()->set_key("a");
    sender->SendEvent(event);

    ASSERT_EQ(key_event_sender->sent_events.size(), 1);
    EXPECT_EQ(key_event_sender->sent_events[0].key(), "a");
}

TEST(InProcessRtcServiceTest, InputSenderDispatchesTouchAndMouseEvent) {
    ::goldfish::display::test::FakeMultiDisplay fake_multidisplay(
            ::goldfish::avd::globalEventLoop());
    TestAvdUniverse universe(&fake_multidisplay, *::goldfish::avd::globalEventLoop());
    auto key_event_sender = std::make_shared<FakeKeyEventSender>();
    auto factory = CreateInProcessInputSenderFactory(fake_multidisplay, key_event_sender);
    auto sender = factory(DataChannelLabel::kInput);
    ASSERT_NE(sender, nullptr);
    EXPECT_TRUE(sender->Start().ok());

    InputEvent touch_event;
    auto* touch = touch_event.mutable_touch_event()->add_touches();
    touch->set_x(100);
    touch->set_y(200);
    touch->set_identifier(0);
    touch->set_pressure(100);
    sender->SendEvent(touch_event);

    InputEvent mouse_event;
    mouse_event.mutable_mouse_event()->set_x(50);
    mouse_event.mutable_mouse_event()->set_y(60);
    sender->SendEvent(mouse_event);

    auto display = fake_multidisplay.GetDisplay<::goldfish::display::test::ActiveFakePixmanDisplay>(
            fake_multidisplay.GetDisplay(0));

    EXPECT_GT(display->evdevs.size(), 0);
    bool has_touch = false;
    for (const auto& ev : display->evdevs) {
        if (ev.type == 3) has_touch = true;  // EV_ABS
    }
    EXPECT_TRUE(has_touch);

    ASSERT_EQ(display->mouse_events.size(), 1);
    EXPECT_EQ(display->mouse_events[0].x, 50);
    EXPECT_EQ(display->mouse_events[0].y, 60);
}

TEST(InProcessRtcServiceTest, InputSenderV2DispatchesToAHumanSessionClosedWithTheChannel) {
    ::goldfish::display::test::FakeMultiDisplay fake_multidisplay(
            ::goldfish::avd::globalEventLoop());
    TestAvdUniverse universe(&fake_multidisplay, *::goldfish::avd::globalEventLoop());
    auto key_event_sender = std::make_shared<FakeKeyEventSender>();
    auto dispatcher = std::make_shared<RecordingInputDispatcher>();
    auto factory =
            CreateInProcessInputSenderFactory(fake_multidisplay, key_event_sender, dispatcher);
    auto sender = factory(DataChannelLabel::kInputV2);
    ASSERT_NE(sender, nullptr);
    EXPECT_TRUE(sender->Start().ok());
    ASSERT_TRUE(dispatcher->opened_with.has_value());
    EXPECT_TRUE(dispatcher->opened_with->human);
    EXPECT_EQ(dispatcher->opened_with->source_id, 0);
    EXPECT_TRUE(dispatcher->opened_with->reject_down_on_held);

    ::android::emulation::v2::InputEvent v2_event;
    auto* pointer = v2_event.mutable_pointer();
    pointer->set_display_id(0);
    pointer->set_action(::android::emulation::v2::PointerAction::POINTER_ACTION_DOWN);
    auto* ptr = pointer->add_pointers();
    ptr->set_pointer_id(0);
    ptr->set_x(0.5F);
    ptr->set_y(0.5F);
    ptr->set_pressure(1.0F);

    sender->SendV2Event(v2_event);
    ::android::emulation::v2::InputEvent text_event;
    text_event.mutable_text()->set_text("测试_🚀_é");
    sender->SendV2Event(text_event);
    EXPECT_THAT(dispatcher->calls,
                ::testing::ElementsAre("pointer 7 display 0", "text 7 测试_🚀_é"));

    sender.reset();
    EXPECT_THAT(dispatcher->calls,
                ::testing::ElementsAre("pointer 7 display 0", "text 7 测试_🚀_é", "close 7"));
    EXPECT_TRUE(key_event_sender->sent_events.empty());
}

TEST(InProcessRtcServiceTest, InputSenderV2DropsInvalidEventsBeforeDispatch) {
    ::goldfish::display::test::FakeMultiDisplay fake_multidisplay(
            ::goldfish::avd::globalEventLoop());
    TestAvdUniverse universe(&fake_multidisplay, *::goldfish::avd::globalEventLoop());
    auto key_event_sender = std::make_shared<FakeKeyEventSender>();
    auto dispatcher = std::make_shared<RecordingInputDispatcher>();
    auto factory =
            CreateInProcessInputSenderFactory(fake_multidisplay, key_event_sender, dispatcher);
    auto sender = factory(DataChannelLabel::kInputV2);
    ASSERT_NE(sender, nullptr);
    EXPECT_TRUE(sender->Start().ok());

    ::android::emulation::v2::InputEvent mouse_event;
    auto* mouse_pointer = mouse_event.mutable_pointer();
    mouse_pointer->set_display_id(0);
    mouse_pointer->set_action(::android::emulation::v2::PointerAction::POINTER_ACTION_DOWN);
    auto* mouse_ptr = mouse_pointer->add_pointers();
    mouse_ptr->set_pointer_id(0);
    mouse_ptr->set_tool_type(::android::emulation::v2::ToolType::TOOL_TYPE_MOUSE);
    sender->SendV2Event(mouse_event);

    ::android::emulation::v2::InputEvent scroll_event;
    scroll_event.mutable_pointer()->set_display_id(0);
    scroll_event.mutable_pointer()->set_action(
            ::android::emulation::v2::PointerAction::POINTER_ACTION_SCROLL);
    scroll_event.mutable_pointer()->set_scroll_y(1.0F);
    sender->SendV2Event(scroll_event);

    ::android::emulation::v2::InputEvent invalid_key_event;
    invalid_key_event.mutable_key()->set_action(
            ::android::emulation::v2::KeyAction::KEY_ACTION_PRESS);
    invalid_key_event.mutable_key()->set_dom_code("a");
    sender->SendV2Event(invalid_key_event);

    EXPECT_THAT(dispatcher->calls, ::testing::IsEmpty());
}

}  // namespace
}  // namespace android::emulation::control
