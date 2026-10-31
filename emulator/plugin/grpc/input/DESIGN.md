# Input Pipeline Design (`emulator/plugin/grpc/input`)

**Location:** `hardware/generic/goldfish/emulator/plugin/grpc/input`
**Namespaces:** `goldfish::grpc::v2`, `android::emulation::control`
**Related:** `emulator/plugin/grpc/keyboard` (key conversion and QEMU keyboard sink), `emulator/plugin/grpc/services/v2/input` (v2 `InputService`), `emulator/plugin/grpc/services` (v1 `EmulatorController` and WebRTC input bridges), `emulator/plugin/display` (per-display virtio-input sinks and active-generation tracking)

---

## 0. TL;DR

Every input path into the emulator—Android Studio's v1 `streamInputEvent` and `injectWheel` streams, v1 unary RPCs (`sendKey`, `sendMouse`, `sendTouch`), WebRTC `input` (v1) and `input_v2` data channels, and the AEMU v2 `InputService` (`SendInputEvents`, `StreamInputEvents`)—routes through per-client **`DispatcherSession`** handles into a single **`InputDispatcher`** actor running on the QEMU event loop.

```text
  v1 gRPC (stream / unary) ──┐
  WebRTC "input" (v1) ───────┴─► V1InputTranslator ──┐
                                                     ├─► DispatcherSession ──► InputDispatcher (QEMU loop)
  v2 gRPC InputService ──────┬─► V2InputValidator ───┘                            │
  WebRTC "input_v2" (v2) ────┘                                                    ├──► MultiDisplayTouchDevices ──► virtio-input-android
                                                                                  ├──► QemuKeyboardSink ──────────► QEMU keyboard
                                                                                  └──► Activity outbox + thread ──► StreamInputActivity
```

Why this design:
1. **Single owner of guest device state:** Only `InputDispatcher` allocates multi-touch slots (`ContactTable`) and tracks held keys (`key_holders_`), preventing concurrent human and automated sessions from overwriting each other's touch slots or releasing keys another session still holds.
2. **Complete, ordered observability:** Because every input path converges at `InputDispatcher`, `InputService.StreamInputActivity` observes both human operator input (source `0`) and automated agent input in the exact sequence the guest receives it.
3. **API semantics decided at the boundary:** Legacy v1 peculiarities (such as emulating `MouseEvent` as a single-finger touch on pointer `0`) are translated once in `V1InputTranslator` into the canonical v2 `InputEvent` vocabulary.
4. **Lock-free actor execution on the QEMU loop:** Running all device state mutations and evdev report writes on the QEMU event loop guarantees atomic multi-touch reports (`SYN_REPORT`), eliminates cross-layer lock-ordering hazards with QEMU/display locks, and keeps subscriber notifications off the loop via a dedicated activity delivery thread.

---

## 1. Where to Start Reading

| File | What it is | Read it when... |
| :--- | :--- | :--- |
| [`input_dispatcher_interface.h`](input_dispatcher_interface.h) | `IInputDispatcher`, `SessionOptions`, `DispatchResult`, and `SessionResetTracker` (no QEMU or display header dependencies) | You want to dispatch input, subscribe to `activity()`, or mock the dispatcher. |
| [`dispatcher_session.h`](dispatcher_session.h) / [`.cc`](dispatcher_session.cc) | `DispatcherSession`: RAII per-client session handle | You are wiring a new gRPC or WebRTC input endpoint. |
| [`input_dispatcher.h`](input_dispatcher.h) / [`.cc`](input_dispatcher.cc) | `InputDispatcher`: QEMU-loop actor, key holder refcounting, idle timers, resync hooks, and activity delivery thread | You are changing dispatch ordering, session lifecycle, resync, or activity reporting. |
| [`contact_table.h`](contact_table.h) / [`.cc`](contact_table.cc) | `ContactTable`: per-device multi-touch slot allocator with human-slot reservation | You are debugging multi-touch slot allocation or `RESOURCE_EXHAUSTED` errors. |
| [`touch_encoding.h`](touch_encoding.h) / [`.cc`](touch_encoding.cc) | Pure functions encoding v2 `PointerEvent::Pointer` contacts into Linux Multi-Touch Protocol B `EvDevEvent` sequences | You are touching pressure/size scaling, stylus/eraser `EV_KEY` state, tilt, or palm cancellation. |
| [`touch_device.h`](touch_device.h) & [`multi_display_touch_devices.h`](multi_display_touch_devices.h) | `TouchDevices` / `TouchDeviceSink` interfaces and `MultiDisplayTouchDevices` adapter over `IMultiDisplay` | You are working on multi-display routing, foldable posture remapping, or driver-active generation checks. |
| [`keyboard_sink.h`](keyboard_sink.h) & [`qemu_keyboard_sink.h`](../keyboard/include/android/emulation/control/keyboard/qemu_keyboard_sink.h) | `KeyboardSink` interface and QEMU keyboard implementation | You are changing key press/release injection or text typing. |
| [`v1_input_translator.h`](v1_input_translator.h) / [`.cc`](v1_input_translator.cc) | `V1InputTranslator` and `SingleFingerMouseTranslator`: stateful per-stream v1 → v2 translator | You are modifying v1 `MouseEvent`, `TouchEvent`, `PenEvent`, `KeyboardEvent`, or raw `AndroidEvent` handling. |
| [`v2_input_validator.h`](v2_input_validator.h) / [`.cc`](v2_input_validator.cc) | `V2InputValidator`: capability and bounds checker shared by gRPC v2 and WebRTC `input_v2` | You are adding a new v2 input capability or changing validation rules. |

### Build Layering

`input_dispatcher_interface` is intentionally free of QEMU and `plugin/display` dependencies so `AvdUniverse` (`//emulator/plugin/avd`) can expose `GetInputDispatcher()` without circular dependencies. `AvdExtendedUniverse` constructs the concrete `InputDispatcher`, wiring it to `MultiDisplayTouchDevices`, `QemuKeyboardSink`, and the QEMU `EventLoop`.

---

## 2. Objectives, Principles, and Non-Goals

### 2.1 Objectives

- **Unified dispatch and observation:** Every touch, stylus, key, and text event reaching the guest passes through `InputDispatcher` and is published once, in dispatch order, to `StreamInputActivity`.
- **Isolated per-session state:** Concurrent clients (Android Studio, WebRTC browser sessions, and v2 automation agents) allocate independent slots and hold keys independently; when a client disconnects or times out, only its own contacts and keys are released.
- **Atomic evdev reports:** Every pointer update writes its `EV_ABS` / `EV_KEY` events and trailing `EV_SYN/SYN_REPORT` synchronously in a single QEMU-loop task so concurrent sessions never interleave inside a report.
- **Self-healing across guest transitions:** Guest driver resets, display removal/re-addition, and snapshot loads automatically resynchronize host slot/key state with the guest and notify observers.

### 2.2 Core Principles

1. **Decide meaning once, at the API boundary.** Each transport translates or validates its wire protocol into canonical v2 `InputEvent` messages before reaching `InputDispatcher`.
2. **One owner of device state.** Nothing outside `InputDispatcher` writes multi-touch slot state to guest touch devices.
3. **Observation cannot be bypassed.** The actor that writes to the guest device stamps and sequences `InputActivity`.
4. **Configure behavior at construction, not per event.** Device capabilities (`V2InputValidator::Options`), session policies (`IInputDispatcher::SessionOptions`), and mouse translation policies (`MouseEventTranslator`) are bound when objects are constructed rather than branching on display IDs or guest flags per event.

### 2.3 Non-Goals

- **Dedicated relative-mouse or touchpad virtio devices:** Mouse input is emulated as a single-finger touch contact (`SingleFingerMouseTranslator`). Dedicated mouse/trackpad devices belong to future v2 capabilities.
- **Wheel and rotary input without a guest sink:** v1 `WheelEvent` is ignored (`VLOG(1)`), and v2 rotary `AxisEvent` is rejected by `V2InputValidator` unless a hardware rotary sink is configured (`hw_rotary_input`).
- **Preempting agent touches on human contact:** Rather than cancelling active agent gestures when the human touches the screen, `ContactTable` reserves `kReservedHumanSlots = 2` slots per device exclusively for human sessions (`SessionOptions::human = true`).

---

## 3. Architecture and Threading Model

```text
  ┌──────────────────────────────────────────────────────────────────────────────────────┐
  │ Caller Threads (gRPC callback / WebRTC / worker threads)                             │
  │                                                                                      │
  │   v1 EmulatorController & WebRTC "input" ──► V1InputTranslator (per stream/session)  │
  │                                                │              ▲                      │
  │   v2 InputService & WebRTC "input_v2" ─────► V2InputValidator │ SessionResetTracker  │
  │                                                │              │ (atomic version)     │
  │                                                ▼              │                      │
  │                                         DispatcherSession     │                      │
  └────────────────────────────────────────────────┼──────────────┼──────────────────────┘
                                 PostOperation()   │              │
                                                   ▼              │
  ┌───────────────────────────────────────────────────────────────┼──────────────────────┐
  │ QEMU Event Loop Thread (InputDispatcher Actor)                │                      │
  │                                                               │                      │
  │   Pending queue (capped at 256/session)                       │                      │
  │     └──► InputDispatcher::ApplyPointer / ApplyKey / ApplyText ┴── Resync / Reset     │
  │            ├──► ContactTable (11 slots/device, 2 human-reserved)                     │
  │            ├──► key_holders_ (session set per evdev code)                            │
  │            ├──► MultiDisplayTouchDevices::WriteReport ──► virtio-input-android       │
  │            └──► QemuKeyboardSink ───────────────────────► QEMU Virtio Keyboard       │
  └────────────────────────────────────────────────┬─────────────────────────────────────┘
                   Report() (sequence + timestamp) │
                                                   ▼
  ┌──────────────────────────────────────────────────────────────────────────────────────┐
  │ Activity Delivery Thread                                                             │
  │                                                                                      │
  │   Sequenced outbox_ ──► InputDispatcher::DeliverActivity() ──► StreamInputActivity   │
  └──────────────────────────────────────────────────────────────────────────────────────┘
```

### Threading & Lock Boundaries

- **Caller threads:** Run `V2InputValidator::Validate()` or `V1InputTranslator::Translate*()`, then call `DispatcherSession::Dispatch()`. `InputDispatcher` records the operation in `pending_` under a leaf mutex (`pending_mutex_`), enforces the per-session in-flight limit (`kDefaultMaxPendingPerSession = 256`), and posts the task to the QEMU `EventLoop`.
- **QEMU event loop thread (`loop_`):** Owns all mutable device state (`contacts_`, `sessions_`, `stylus_contacts_`, `key_holders_`, `active_generations_`, `stale_slots_`) without locks. It executes each operation synchronously, writes the evdev report via `TouchDeviceSink::WriteReport` or `KeyboardSink`, appends the resulting `SequencedActivity` to `outbox_` under leaf mutex `outbox_mutex_`, and invokes the caller's `DispatchCallback`.
- **Activity delivery thread (`delivery_thread_`):** Drains `outbox_` in strict sequence order and invokes `activity()` subscribers outside both `outbox_mutex_` and the QEMU loop (and therefore outside the BQL).
- **Lock-ordering rule:** `pending_mutex_` and `outbox_mutex_` are independent leaf locks; neither is ever held while acquiring the other, invoking a `DispatchCallback`, writing to a device sink, or firing an activity listener.

---

## 4. Core Subsystems

### 4.1 Touch Slot Ownership and Foldable Binding (`ContactTable` & `TouchDevices`)

Each physical display owns a `virtio-input-android` multi-touch device advertising `ABS_MT_SLOT` `0..10` (`kTouchDeviceSlots = 11`). `ContactTable` maps logical pointers to physical device slots:

- **Logical contact identity (`ContactKey`):** Keyed by `{session, display_id, pointer_id}`, where `display_id` is the display requested by the client. Scoping by `display_id` allows a single session (such as Android Studio's shared `streamInputEvent` stream) to drive concurrent gestures with `pointer_id = 0` on display 0 and display 1 without colliding.
- **Physical device binding (`Contact`):** When a new contact goes `DOWN` (or `HOVER_ENTER` / `HOVER_MOVE`), `TouchDevices::Resolve(display_id)` resolves the target physical `TouchDeviceId` once (for example, remapping display `0` to display `1` when a foldable AVD is folded). The contact remains bound to that `TouchDeviceId` for all subsequent `MOVE`, `UP`, and `CANCEL` events until released—even if the foldable posture changes mid-drag. `InputActivity` always reports the client's *requested* `display_id` so recorded gestures replay independent of posture.
- **Human slot reservation:** Of the 11 slots per device, `kReservedHumanSlots = 2` are reserved for sessions opened with `SessionOptions::human = true` (all v1 and WebRTC `source_id == 0` sessions). Non-human sessions can occupy at most `9` slots per device, guaranteeing the human operator can always perform a two-finger gesture even if automation agents saturate their quota.
- **Slot number as tracking ID:** The allocated slot index (`0..10`) is also written as `ABS_MT_TRACKING_ID`, satisfying Linux Multi-Touch Protocol B.

### 4.2 Multi-Touch Protocol B and Stylus Encoding (`touch_encoding`)

`touch_encoding.cc` converts v2 `PointerEvent::Pointer` structs into raw `EvDevEvent` sequences:

- **Slot selection order:** `AppendContactDownOrMove` always emits `EV_ABS ABS_MT_SLOT <slot>` *before* `ABS_MT_TRACKING_ID`, position, tool type, pressure, and touch major/minor so multi-finger reports never overwrite the previously active slot in the guest kernel.
- **Coordinate, size, and pressure scaling:**
  - Normalized `x`, `y`, and `size` in `[0.0, 1.0]` scale to `0..0x7FFF` (`kEvAbsMax`). When `size <= 0`, `ABS_MT_TOUCH_MAJOR` and `ABS_MT_TOUCH_MINOR` default to `kDefaultTouchMajor = 0x0500` (~4% of the panel).
  - Normalized `pressure` in `[0.0, 1.0]` scales to `0..0x400` (`kMtsPressureMax`). Hover actions (`HOVER_ENTER`, `HOVER_MOVE`) encode `0` pressure.
  - `orientation_radians` in `[-π/2, π/2]` converts to integer degrees `[-90, 90]` on `ABS_MT_ORIENTATION`.
  - For tilted stylus/eraser contacts (`tilt_radians > 0`), 3D spherical `(tilt_radians, orientation_radians)` decomposes into planar `ABS_TILT_X` and `ABS_TILT_Y` degrees `[-90, 90]`. *(Note: while `virtio-input-android` advertises `ABS_TILT_X` and `ABS_TILT_Y`, Android's `MultiTouchInputMapper` currently ignores both axes on multi-touch devices across API 24–36; see §8.2 G6 and §8.3.4 Trap 4.)*
- **Stylus and eraser tool encoding (`MapToolType`):**
  - Linux evdev has no `MT_TOOL_ERASER` constant. In Android's `MultiTouchInputMapper::syncTouch`, `MT_TOOL_PEN` maps directly to `MotionEvent.TOOL_TYPE_STYLUS`, whereas `MT_TOOL_MAX` maps to `ToolType::UNKNOWN` and causes `TouchInputMapper::cookPointerData` to fall back to `mTouchButtonAccumulator.getToolType()`, which reads `EV_KEY BTN_TOOL_RUBBER = 1` to produce `MotionEvent.TOOL_TYPE_ERASER`.
  - Accordingly, `TOOL_TYPE_STYLUS` emits `BTN_TOOL_RUBBER = 0` + `ABS_MT_TOOL_TYPE = MT_TOOL_PEN`, and `TOOL_TYPE_ERASER` emits `BTN_TOOL_RUBBER = 1` + `ABS_MT_TOOL_TYPE = MT_TOOL_MAX`, alongside `EV_KEY BTN_STYLUS` (`1` when `BUTTON_MASK_STYLUS_PRIMARY` is set).
  - Because `BTN_TOOL_RUBBER` and `BTN_STYLUS` are global per-device keys rather than per-slot properties, `InputDispatcher` tracks active stylus contacts per device (`stylus_contacts_`) and emits `AppendStylusKeysReset` (`BTN_TOOL_RUBBER = 0`, `BTN_STYLUS = 0`) when the last stylus/eraser contact lifts or switches tool type.
- **Two-frame palm cancellation (`AppendContactCancel`):**
  - Android's `MultiTouchMotionAccumulator` clears a slot immediately upon seeing `ABS_MT_TRACKING_ID = -1` before `SYN_REPORT` syncs `TouchInputMapper`. Putting `MT_TOOL_PALM` and `ABS_MT_TRACKING_ID = -1` in the same frame would cause Android to treat the lift as a normal tap/release instead of a cancel.
  - Therefore, cancelling contacts (on `POINTER_ACTION_CANCEL`, session close, idle expiry, or stale-slot flush) writes **two back-to-back reports**:
    1. **Palm frame (`palm_events` + `SYN_REPORT`):** `ABS_MT_SLOT <slot>`, `ABS_MT_TOOL_TYPE MT_TOOL_PALM`.
    2. **Lift frame (`lift_events` + `SYN_REPORT`):** `ABS_MT_SLOT <slot>`, `ABS_MT_TRACKING_ID -1` (plus optional stylus key reset).
  - *(Note: per-pointer `MT_TOOL_PALM` cancellation requires Android 12+ [`API 31+`]; see §8.2 G7 and §8.3.4 Trap 1 for behavior on API 24–30.)*

### 4.3 Keyboard Reference Counting (`key_holders_` & `KeyboardSink`)

`InputDispatcher` resolves v2 `KeyEvent`s (`dom_code`, `android_keycode`, or `evdev_code`) to Linux evdev keycodes via `ResolveEvdevKeyCode` and tracks the set of sessions holding each key (`key_holders_[code]`):

- **`KEY_ACTION_DOWN`:** Adds the session to `key_holders_[code]` and *always* forwards a press to `KeyboardSink::WriteKey(code, true)` so client key-autorepeat reaches the guest.
- **`KEY_ACTION_UP`:** Removes the session from `key_holders_[code]` (if present) and writes `KeyboardSink::WriteKey(code, false)` to the guest **only when `key_holders_[code]` becomes empty**. If another session still holds the key, the guest key remains down, while the releasing session's `UP` is still recorded on `StreamInputActivity`.
- **`KEY_ACTION_PRESS`:** Executes `DOWN` followed immediately by `UP`.
- **Guest sink (`QemuKeyboardSink`):** Converts Linux evdev keycodes through QEMU's `QKeyCode` table (`input-keymap-linux-to-qcode`) and injects them into `virtio-keyboard-pci` (`"QEMU Virtio Keyboard"`). *(See §8.2 G4 and §8.3.4 Trap 2 for Android-specific evdev keycodes not present in `QKeyCode` and `.idc` / `.kl` lookup differences across API levels.)*

### 4.4 v1-to-v2 Translation (`V1InputTranslator`)

Each v1 input endpoint owns a stateful `V1InputTranslator` that converts legacy `android.emulation.control` messages into canonical v2 `InputEvent`s:

| v1 Message | Translation Behavior |
| :--- | :--- |
| **`MouseEvent`** | Handled by `SingleFingerMouseTranslator` on **pointer ID `0`** (`kMouseAndTouchZeroPointerId`): primary button (bit `0`) transitions pointer `0` on that display through `DOWN` → `MOVE` → `UP` with `TOOL_TYPE_FINGER` and pressure `1.0`. Hover moves (`buttons == 0` while pointer `0` is not held) and non-primary buttons are ignored. |
| **`TouchEvent`** | Identifiers `0..9` (`kMaxTouchPointers = 10`) map directly to session pointer IDs `0..9` when available; other external IDs are dynamically assigned a free pointer ID in `0..9`. Sharing pointer `0` between `MouseEvent` and `TouchEvent` allows Android Studio's Ctrl-pinch overlay to transition seamlessly mid-drag. Simultaneous new presses and releases in one v1 packet are split into two ordered v2 `PointerEvent`s (`DOWN`, then `MOVE`/`UP`). |
| **`PenEvent`** | Mapped to dedicated session **pointer ID `10`** (`kPenPointerId`) with `TOOL_TYPE_STYLUS` (or `TOOL_TYPE_ERASER` when `rubber_pointer` is set) and `BUTTON_MASK_STYLUS_PRIMARY` when `button_pressed` is set: positive pressure (`pressure > 0`) transitions pointer `10` through `DOWN` → `MOVE` → `UP`, while hover events (`pressure == 0` while pointer `10` is not held) are ignored. |
| **`KeyboardEvent`** | Translates `key` (DOM non-printable names, printable ASCII replaying `ascii_to_qcode` Shift/Alt modifier sequences, or DOM code strings), `text`, and `keyCode` (`Usb`, `Evdev`, `XKB`, `Win`, `Mac`) in order. |
| **`AndroidEvent`** | Raw `(type, code, value)` tuples are buffered per display (up to `kMaxBufferedAndroidEvents = 256`) until `EV_SYN/SYN_REPORT`, then flushed via `IDisplay::SendEvDevReport` after stripping multi-touch slot codes (`ABS_MT_*`, `BTN_TOUCH`, `BTN_TOOL_*`) that would desynchronize `ContactTable`. |
| **`WheelEvent` / XR** | Logged at `VLOG(1)` and dropped; v1 streams never fail on unsupported event types. |

#### Synchronizing Translator State Across Resets (`SessionResetTracker`)

Because `V1InputTranslator` tracks which pointers are held per display on the caller's thread so it can classify the next v1 packet as `DOWN` vs `MOVE` vs `UP`, it must know if `InputDispatcher` asynchronously cleared the session's contacts (on idle timeout, snapshot load, display removal, or guest driver reset). `DispatcherSession` shares a `SessionResetTracker` with `InputDispatcher`: on the hot path, `V1InputTranslator::SyncResetTracker` performs a single lock-free `std::memory_order_acquire` load of `tracker.Version()`, taking the snapshot lock only when a full or per-display reset generation advanced.

---

## 5. Session Lifecycles, Pacing, and Error Handling

| Endpoint | Session Ownership & Options | Idle Timeout | Error Policy |
| :--- | :--- | :--- | :--- |
| **v1 `streamInputEvent` / `injectWheel`** | One `DispatcherSession` + `V1InputTranslator` per stream (`human = true`, `source_id = 0`, `reject_down_on_held = false`), closed in reactor `OnDone()` | None (stream lifetime) | Unsupported/rejected events logged at `VLOG(1)` and dropped; stream stays open |
| **v1 Unary RPCs (`sendKey`, `sendMouse`, `sendTouch`)** | Shared `unary_session_` + `unary_translator_` on `EmulatorControllerImpl`, serialized by `unary_input_mutex_` (`human = true`, `source_id = 0`) | **60 s** | Releases abandoned drags/keys after 60 s of inactivity |
| **WebRTC `input` (v1)** | One `DispatcherSession` + `V1InputTranslator` per data channel in `InProcessInputSender` (`human = true`, `source_id = 0`) | None (channel lifetime) | Dropped at `VLOG(1)` |
| **WebRTC `input_v2` (v2)** | One `DispatcherSession` per data channel (`human = true`, `source_id = 0`, `reject_down_on_held = true`) | None (channel lifetime) | Validated by `V2InputValidator`; rejections logged at `VLOG(1)` |
| **v2 `StreamInputEvents`** | Per-`(stream, source_id)` `DispatcherSession` map owned by `StreamInputReader` (`human = (source_id == 0)`, `reject_down_on_held = true`) | None (stream lifetime) | Synchronous validation failure or async loop failure (`NotifyOnFirstFailure`) terminates the stream with status + event index |
| **v2 `SendInputEvents` (identified: `source_id != 0` or `x-client-id`)** | Persistent per-source `DispatcherSession` in `InputServiceImpl::batch_sessions_` (`human = false`, `reject_down_on_held = true`) | **60 s** (`held_input_idle_timeout_ms`) | Multi-RPC gestures stay held across calls; returns first failure with `events[i]` index |
| **v2 `SendInputEvents` (anonymous: `source_id == 0`, no `x-client-id`)** | Temporary `anonymous_session` per RPC call (`human = true`, `reject_down_on_held = true`) | Closed when RPC returns | Contacts and keys left down in the batch are released when the call finishes |

---

## 6. Resync, Lifecycle Transitions, and Shutdown

Guest input state can change outside client control. Every transition marshals onto the QEMU loop so state updates remain lock-free and serialized with normal input:

1. **Guest virtio-input driver reset or re-probe (`VirtioInputActiveTracker`):**
   - `virtio-input-android` increments an atomic active-generation counter and fires an event whenever the guest driver activates, resets, or re-probes.
   - `MultiDisplayTouchDevices` subscribes to these transitions and triggers `InputDispatcher::OnDeviceActiveChanged()`, which posts `ResyncDevices()` onto the loop (and `ApplyPointer` / `CloseSession` also call `ResyncDevices()` before touching any device).
   - `ResyncDevices()` clears the affected device's contacts in `ContactTable` *without* writing to the guest (whose driver already forgot them), records a display reset on each affected session's `SessionResetTracker`, and emits synthesized `CANCEL` activities.
2. **Display removal and re-addition (`OnDisplayRemoved` / `OnDisplayAdded`):**
   - When a secondary display is removed, `OnDisplayRemoved(device)` clears its contacts in `ContactTable`, notifies `SessionResetTracker`, emits synthesized `CANCEL` activities, and saves the previously occupied slot bitset in `stale_slots_[device]`.
   - Because secondary virtio-input devices remain realized in QEMU across display removal/re-creation, if the display is re-added (`OnDisplayAdded`) without its guest driver having reset (same `ActiveGeneration`), `FlushStaleSlots` palm-cancels those stale slots on the guest device before new touches are written.
3. **Snapshot load (`OnSnapshotLoaded`):**
   - After a snapshot loads and the VM resumes, `ResetAfterSnapshotLoad()` runs on the QEMU loop: it lifts all slots (`0..10`) and resets stylus keys on every active touch device, resets `KeyboardSink` (`QKbdState` and held keys), clears `ContactTable` and `key_holders_`, records a full reset on every session's `SessionResetTracker`, and emits synthesized `CANCEL` and `KEY_ACTION_UP` activities.
4. **Emulator shutdown (`InputDispatcher::Shutdown()`):**
   - Invoked explicitly at the start of `grpc_main_loop_exit_notify` before `grpc_server->Shutdown()`.
   - Completes all queued operations in `pending_` with `UNAVAILABLE` on the calling thread (unblocking any waiting `SendInputEvents` calls), flushes and joins `delivery_thread_`, and makes all subsequent `OpenSession`, `CloseSession`, and `Dispatch*` calls safe no-ops so long-lived holders (such as WebRTC channels torn down after the gRPC server) can close sessions safely after the QEMU loop stops.

---

## 7. Invariants and Assumptions

### Invariants

- **I1 (Slot fidelity):** For every active `TouchDeviceId`, `ContactTable` matches the active slots in the guest kernel after all written reports are applied, except between a guest driver active-generation transition and the `ResyncDevices()` check that clears it.
- **I2 (Report atomicity):** Every pointer operation writes at most one evdev report per device (or two consecutive reports when palm-cancelling), each terminated by `EV_SYN/SYN_REPORT` inside a single QEMU-loop task.
- **I3 (Key holder conservation):** An evdev keycode is present in `key_holders_` with a non-empty session set if and only if the last event written for that keycode to `KeyboardSink` was a key-down (since the last keyboard reset).
- **I4 (Per-session FIFO):** Operations dispatched by a single session execute on the QEMU loop in the exact order they were posted.
- **I5 (Activity sequence order):** `outbox_` assigns strictly increasing `sequence` numbers on the QEMU loop at write time, and `delivery_thread_` delivers them to subscribers in that exact order.

### Assumptions

- **Natural panel coordinates:** Both v1 and v2 pointer coordinates are expressed in the display panel's natural (unrotated) coordinate frame; guest Android `InputDispatcher` applies display rotation.
- **Single-threaded per-session callers:** `DispatcherSession::Dispatch` is thread-safe, but callers that care about event ordering within a session dispatch sequentially from one stream/channel reader.
- **11 hardware slots per display:** `kTouchDeviceSlots = 11` matches `MTS_POINTERS_NUM = 10` (`ABS_MT_SLOT` `0..10`) in `virtio-input-android`, with `2` reserved for `human = true` sessions and `9` shared across automated sessions.

---

## 8. Guest Android Routing (API 24–36) & `qemu2` Gap Analysis

This section compares the classic `qemu2` (`external/qemu`) input stack against the `qemu-next` `InputDispatcher` architecture in §§0–7, then maps how **Android guest API levels (API 24–36)**, **`FeatureControl` flags**, and **`hw.*` / AVD form-factor configurations** govern event routing inside the Linux kernel and Android `InputReader`.

---

### 8.1 Architectural Comparison (`qemu2` vs. `qemu-next`)

In classic `qemu2`, input is **not** funneled through a single host-side state owner or static VirtIO device topology. Instead, Qt UI (`SkinEvent` queue), gRPC v1 (`*EventSender`), WebRTC (`EventForwarder` / `EventDispatcher`), and Telnet (`console.cpp`) converge on `QAndroidUserEventAgent` (`android-qemu2-glue/qemu-user-event-agent-impl.c`), which branches per event across **conditional guest hardware and pipe sinks** gated by `avdInfo_getApiLevel()`, `FeatureControl` (`VirtioInput`, `VirtioMouse`, `VirtioTablet`, `VirtioDualModeMouse`), `hw.*` config properties, and `AvdFlavor`:

```text
  qemu2 Entry Points               Host Routing                 Conditional Guest Sinks
  ──────────────────               ────────────                 ───────────────────────
  Qt UI / SkinWindow ──────┬──► QAndroidUserEventAgent ──┬──► goldfish-events ("qwerty2" SysBus)
  gRPC v1 (*EventSender) ──┤          (under BQL)        │      API <= 29 touch/keys/EV_SW;
  WebRTC (kbd/mouse/touch) ┤                             │      hw.keyboard=no fallback keys
  Telnet (event commands) ─┘                             ├──► virtio_input_multi_touch_1..11
                                                         │      API >= 30 touch + pen (slot 10) + SW_LID
                                                         ├──► virtio_input_multi_touch_touchpad_0
                                                         │      hw.touchpad0 / AVD_GLASSES
                                                         ├──► virtio-keyboard-pci ("qwerty2")
                                                         │      hw.keyboard=yes + mandatory_android_keys
                                                         ├──► virtio-mouse / tablet / dual-mode-mouse
                                                         │      VirtioMouse / VirtioTablet / XR
                                                         └──► virtio_input_rotary / goldfish_rotary
                                                                hw.rotaryInput / AVD_WEAR
  Qt UI / gRPC v1 (XR) ────────► QEMUD "xr_service" pipe ───► Guest Android XR runtime
```

By contrast, `qemu-next` (`DisplayDevice::getQemuParameters` in `emulator/launcher/launch_qemu/display_device.cc`) **unconditionally** instantiates only:
- 1× `virtio-keyboard-pci` (`name = "QEMU Virtio Keyboard"`, `0x0627:0x0001`), and
- 11× `virtio-input-android-pci` (`name = "virtio_input_multi_touch_1"` .. `"_11"`, `0x0000:0x0000`),
with no checks on `ApiLevel`, `FeatureFlags` (`VIRTIO_INPUT`, `VIRTIO_MOUSE`, `VIRTIO_TABLET`, `CAR_ROTARY`, and `TV_REMOTE` are marked `IsIgnoredLegacy()` in `feature_flags.cc`), `hw.keyboard`, `hw.screen`, `hw.rotaryInput`, or `hw.touchpad0`.

---

### 8.2 Functional Gap Matrix (`qemu2` vs. `qemu-next`)

The following **13 guest-device and API-level gaps** exist between `qemu2` and the current `qemu-next` device topology (tracked in Buganizer hotlists [9100626](http://b/hotlists/9100626) and [9153807](http://b/hotlists/9153807)):

| # | Capability / Subsystem | `qemu2` (`external/qemu`) Behavior | `qemu-next` Behavior | Affected APIs & Form Factors |
| :---: | :--- | :--- | :--- | :--- |
| **G1**<br>[b/579938225](http://b/579938225) | **Legacy `goldfish-events` (`GFSH0002`, `"qwerty2"`) SysBus device** | Always realized on SysBus (`pc_piix`, `pc_q35`, `arm/virt`). When `!VirtioInput`, drives all multi-touch/single-touch, keyboard, D-pad, trackball, and `EV_SW` switches. When `VirtioInput` is ON and `hw.keyboard=no`, still delivers hardware keys (`KEY_POWER`, `KEY_VOLUME*`, `KEY_HOME`, `KEY_STEM_*`). | **Missing** (`RANCHU_GOLDFISH_EVDEV` commented out on ARM64; absent on x86_64). Only PCI `virtio-input` devices exist. | **Blocker on API 24–28** (and non-refreshed API 29): guest kernels (`3.10`–`4.9`) lack `CONFIG_VIRTIO_INPUT`, leaving AVDs with **zero touch input** (and zero keys on ARM64). |
| **G2**<br>[b/579937533](http://b/579937533) | **True pointer devices (`VirtioMouse`, `VirtioTablet`, `VirtioDualModeMouse`)** | `main.cpp:852-861` instantiates `virtio-mouse-pci` (`REL_X/Y`), `virtio-tablet-pci` (`ABS_X/Y + BTN_TOOL_PEN`), or `virtio-dual-mode-mouse-pci` (`REL + ABS + REL_HWHEEL` + `dual_mode_mouse_driver=1` boot prop) instead of `virtio_input_multi_touch_*`. Preserves hover, `BTN_LEFT/RIGHT/MIDDLE`, wheel, and relative pointer capture. | Explicit Non-Goal (§2.3): `SingleFingerMouseTranslator` (§4.4) maps primary button only to capacitive finger touch on pointer `0`; drops hover (`buttons == 0`), right/middle/back/forward clicks, and relative deltas; `V2InputValidator` rejects `TOOL_TYPE_MOUSE` and `POINTER_ACTION_RELATIVE_MOVE`. | **Desktop / Freeform (API 34–36)**, **Android XR**, **Android TV (`hw.screen=no-touch`)**, and **API 26+ `View.requestPointerCapture()`** (games, remote desktop). |
| **G3**<br>[b/579938318](http://b/579938318) | **Mouse wheel & Wear/Car rotary (`WheelEvent`, `AxisEvent`)** | `WheelEventSender` routes `AVD_WEAR` / `hw.rotaryInput` to `virtio_input_rotary_pci` (`API >= 30`) or `goldfish_rotary` (`GFSH0008`, `API <= 29` x86) via `EV_REL REL_WHEEL`; otherwise routes `(dx, dy)` to `kbd_mouse_wheel_event` (`REL_WHEEL` / `REL_HWHEEL`). | `WheelEvent` is dropped at `VLOG(1)` (§2.3, §4.4); `hw_rotary_input` is hardcoded `false`; no `virtio_input_rotary` or `goldfish_rotary` device exists. | **Wear OS** crown/bezel scrolling, **Android Automotive** rotary controller, and **Desktop/Phone** mouse wheel scrolling (`ACTION_SCROLL`). |
| **G4**<br>[b/579938502](http://b/579938502) | **Keyboard identity (`"qwerty2"` vs. `"QEMU Virtio Keyboard"`) & `QKeyCode` bottleneck** | `qemu2` patches `virtio-keyboard-pci` `ID_NAME` to `"qwerty2"` (`b/136093985`), unions `mandatory_android_keys` into `EV_KEY`, and passes raw 10-bit Linux evdev codes directly without round-tripping through `QKeyCode`. | Uses unpatched `"QEMU Virtio Keyboard"` (`0x0627:0x0001`) and funnels `QemuKeyboardSink` through QEMU's 164-entry PC `QKeyCode` table (`input-keymap-linux-to-qcode`, `len = 581`). | **Silently drops** `KEY_SCALE (120)` (API 36 Desktop Overview), `KEY_OK/CENTER (352)` (TV/D-pad), `ANDROID_KEY_STEM_PRIMARY/1/2/3 (264..267)` (Wear/Glasses), `KEY_CAMERA (212)`, `KEY_SEARCH (217)`, `KEY_MICMUTE (248)`, `KEY_SEND (231)`, `KEY_ALL_APPS (644)`, and DOM `"Home"`/`"End"` (`452`/`453`). Also misses `qwerty2.idc` on **API 30–35** (see §8.3.4). |
| **G5**<br>[b/579937662](http://b/579937662) | **`hw.keyboard=no`, `hw.dPad`, & `qemu.hw.mainkeys` gating** | Only creates `virtio-keyboard-pci` when `hw.keyboard=yes`; gates `LINUX_KEY_CENTER` on `hw.dPad`; sets `qemu.hw.mainkeys` boot prop from `hw.mainKeys`. | Unconditionally creates `virtio-keyboard-pci` even when `hw.keyboard=no`, making Android detect a physical full alphabetic keyboard (which can suppress soft IME pop-up unless `show_ime_with_hard_keyboard` is set). | **Phone / Tablet / Wear AVDs** configured with `hw.keyboard=no`; **API <= 33** navigation bar visibility (`qemu.hw.mainkeys`). |
| **G6**<br>[b/579938636](http://b/579938636) | **3D Stylus Tilt (`ABS_TILT_X` / `ABS_TILT_Y`) on `MultiTouchInputMapper`** | `qemu2` does not advertise `ABS_TILT_X/Y` on `virtio_input_multi_touch` (only `ABS_MT_ORIENTATION` `[0, 90]`, dedicating slot `10` to pen). | §4.2 decomposes `(tilt_radians, orientation_radians)` into `ABS_TILT_X` and `ABS_TILT_Y` on `virtio-input-android`. **However, Android's `MultiTouchInputMapper` never reads `ABS_TILT_X` or `ABS_TILT_Y`** (`outPointer.tiltX = 0; tiltY = 0` in API 24–36)! | **All API levels (API 24–36):** `MotionEvent.AXIS_TILT` is always `0.0f` on multi-touch devices; real 3D stylus tilt in Android requires a separate single-touch `SingleTouchInputMapper` pen device (§8.3.4). |
| **G7**<br>[b/579938004](http://b/579938004) | **Two-frame `MT_TOOL_PALM` cancellation on older APIs** | `qemu2` has no session cancel protocol (lifts slots with `ABS_MT_TRACKING_ID = -1`). | §4.2 emits Frame 1 `MT_TOOL_PALM (2)` then Frame 2 `ABS_MT_TRACKING_ID = -1` on `CANCEL`, session close, or 60 s unary idle timeout. | Works per-pointer on **API 31–36**; aborts **all** pointers on **API 30**; **synthesizes an unintended finger tap (`ACTION_UP`) on API 24–29** because `MT_TOOL_PALM` was unknown prior to Android 11! |
| **G8**<br>[b/579939117](http://b/579939117) | **`EV_SW` switches (`SW_LID`, `SW_HEADPHONE_INSERT`, `SW_MICROPHONE_INSERT`, `SW_TABLET_MODE`) & raw `AndroidEvent`** | `virtio_input_multi_touch_1` advertises `SW_LID (0)`; `FoldableModel` emits `SW_LID` on lid/fold transitions; `goldfish-events` advertises `SW_HEADPHONE_INSERT (2)` & `SW_MICROPHONE_INSERT (4)` on API <= 29 *(note: `qemu2` has a bug on API 30+ where `android_virtio_send_event_as_mt` swallows `SW_HEADPHONE/MIC_INSERT`)*. | §4.4 flushes raw `AndroidEvent` to `IDisplay::SendEvDevReport` (`virtio-input-android`), which only advertises `EV_KEY = {BTN_TOOL_RUBBER, BTN_STYLUS}` and `EV_SW = {SW_LID}`. Guest kernel `is_event_supported()` silently drops all other `EV_KEY`/`EV_SW`/`EV_REL` codes. Also, `FoldableModel` in `qemu-next` **never** emits `SW_LID`. | **API 29–31 lid-based foldables** (`SW_LID`), **API 24–29 headset/mic switches**, and all raw `AndroidEvent` `EV_KEY` injections (`KEY_POWER`, `KEY_VOLUME*`). |
| **G9**<br>[b/579938481](http://b/579938481) | **Multi-touch touchpad (`hw.touchpad0` / `TouchpadEvent`)** | Creates `virtio_input_multi_touch_touchpad_pci_0` (`[0..width] x [0..height]`) and routes `TouchpadEvent` via `TouchpadEventSender`. Mandatory on `AVD_GLASSES` (`hw.screen="no-touch"`, `1543x297`). | `TouchpadEvent` is logged at `VLOG(1)` and dropped (§4.4); no touchpad virtio device exists. | **AI Glasses (`AVD_GLASSES`)** and **API 34+ ChromeOS-gestures `TouchpadInputMapper`** testing. |
| **G10**<br>[b/579938875](http://b/579938875) | **Android XR 6DoF, hand, & eye input (`Xr*Event` -> QEMUD `"xr_service"`)** | `EmulatorService::streamInputEvent` forwards `xr_head_*`, `xr_hand_event`, `xr_eye_event`, and `xr_command` over the QEMUD `"xr_service"` pipe (`XrService.cpp`), paired with `VirtioDualModeMouse`. | Logged at `VLOG(1)` and dropped (§4.4); no `"xr_service"` pipe exists. | **Android XR (`AVD_XR`)** headsets. |
| **G11**<br>[b/579939174](http://b/579939174) | **Display slot 6 (`virtio_input_multi_touch_7.idc`) `pixel_fold` collision** | `qemu2` routes `pixel_fold` outer display (`android_foldable_pixel_fold_second_display_id()`) to the matching secondary display slot. | Assumes `virtio_input_multi_touch_1..11` (`head=0..10`) map uniformly to `display_id` `0..10`. | **API 34 QPR–36:** Guest `/vendor/usr/idc/virtio_input_multi_touch_7.idc` (`head=6`) is hardcoded to `local:4619827551948147201` (`pixel_fold` outer screen), **not** `virtual:com.android.emulator.multidisplay:1234567` (`b/289373564`)! |
| **G12**<br>[b/579938776](http://b/579938776) | **`INPUT_PROP_DIRECT` omitted on `virtio-input-android`** | Neither `qemu2` nor `qemu-next` populates `VIRTIO_INPUT_CFG_PROP_BITS` (`1 << INPUT_PROP_DIRECT`), relying 100% on guest `.idc` files setting `touch.deviceType = touchScreen`. | Same (`virtio_input_android_config.c` omits `VIRTIO_INPUT_CFG_PROP_BITS`). | Any guest image lacking `/vendor/usr/idc/virtio_input_multi_touch_N.idc` (e.g. **early API 29** or custom AOSP/GSI images) classifies the screen as `DeviceMode::POINTER` (floating relative trackpad cursor) instead of a direct touchscreen! |
| **G13**<br>[b/579938482](http://b/579938482) | **Telnet `event` console commands (`emulator/telnet`)** | `event send`, `event text`, `event mouse`, `event types/codes` route through `QAndroidUserEventAgent` with full `EV_KEY`/`EV_ABS`/`EV_REL`/`EV_SW`/`EV_SYN` symbol tables (`hw-events.c`). | `LegacyConsoleBridge` forwards to v1 `EmulatorController` RPCs (entering `InputDispatcher` via `unary_session_`), but its parser silently drops all non-`EV_KEY` tuples in `event send`, fails to parse `EV_SW`, recognizes only 7 `KEY_*` aliases (`KEY_HOME` mapped to `102 = MOVE_HOME`), and hardcodes `display = 0` in `event mouse`. | Automation scripts & CTS harnesses using Telnet `event send` / `event mouse`. |

---

### 8.3 How Android Guest Input Routing Works Across API Levels & Features

Understanding how Android's `EventHub` (`frameworks/native/services/inputflinger/reader/EventHub.cpp`), `InputDevice` (`frameworks/native/libs/input/InputDevice.cpp`), and `InputMapper` subclasses classify and transform events is essential when supporting older API levels (`API 24–36`).

#### 8.3.1 Guest `.idc` / `.kl` / `.kcm` Lookup & Mapper Classification Rules

1. **Why PCI IDs (`1af4:1052`), `serial`, and `phys` (`virtio0/input0`) NEVER match `.idc` files:**
   - Linux `virtio_input.ko` populates `input_dev->id` (`EVIOCGID`) from `VIRTIO_INPUT_CFG_ID_DEVIDS` in VirtIO config space, **not** from PCI Vendor/Device IDs.
   - `virtio-input-android` (like `qemu2`'s `virtio_input_multi_touch`) sets `bustype = BUS_VIRTUAL (0x06), vendor = 0x0000, product = 0x0000, version = 0x0000` (`b/129491066`).
   - `InputDevice.cpp` searches `/product/usr/`, `/system_ext/usr/`, `/odm/usr/`, `/vendor/usr/`, and `/system/usr/` in strict priority order:
     1. `Vendor_%04x_Product_%04x_Version_%04x.{idc,kl,kcm}` (skipped when `vendor == 0 && product == 0`)
     2. `Vendor_%04x_Product_%04x.{idc,kl,kcm}` (skipped when `vendor == 0 && product == 0`)
     3. Canonicalized `EVIOCGNAME` (`VIRTIO_INPUT_CFG_ID_NAME`), replacing characters outside `[A-Za-z0-9-_]` with `'_'` (e.g., `"virtio_input_multi_touch_1"` → `virtio_input_multi_touch_1.idc`, `"QEMU Virtio Keyboard"` → `QEMU_Virtio_Keyboard.idc`).
   - Because Goldfish binds secondary touchscreens via `.idc` `touch.displayId` matched by `ID_NAME` (rather than Cuttlefish's `input-port-associations.xml` matched by `EVIOCGPHYS` `virtio%d/input0`), **Goldfish multi-display touch routing is immune to PCI slot / `virtioN` probe ordering**, *provided* `ID_NAME` is literally `"virtio_input_multi_touch_1"`..`"11"`.

2. **How `EventHub` Selects `InputMapper`s (Multiple Mappers Can Coexist on One Device):**
   - `ABS_MT_POSITION_X + ABS_MT_POSITION_Y` (even **without `BTN_TOUCH`**, as long as `!haveGamepadButtons`) → **`MultiTouchInputMapper`** (`TOUCH | TOUCH_MT`) across **all API levels 24–36** *(or `TouchpadInputMapper` on API 34+ if `INPUT_PROP_POINTER` is set without stylus/finger tool bits)*.
   - `ABS_X + ABS_Y + BTN_TOUCH` (without `ABS_MT_*`) → **`SingleTouchInputMapper`**. Notably, `QEMU Virtio Tablet` (`ABS_X/Y + BTN_TOUCH + BTN_TOOL_PEN + BTN_LEFT/RIGHT/MIDDLE + REL_WHEEL`, no `INPUT_PROP_DIRECT`, no `.idc`) classifies as **`SingleTouchInputMapper` in `DeviceMode::POINTER` (`PointerUsage::STYLUS`)**, giving `AINPUT_SOURCE_MOUSE | AINPUT_SOURCE_STYLUS` with absolute cursor placement, hover, right/middle clicks, and wheel scroll!
   - `REL_X + REL_Y + BTN_MOUSE` → **`CursorInputMapper`** (`QEMU Virtio Mouse`). `QEMU Virtio Dual Mode Mouse` advertises both sets of capabilities and attaches **both `CursorInputMapper` and `SingleTouchInputMapper`** on the same `InputDevice`.
   - `EV_SW` (any bit set) → **`SwitchInputMapper`** (attached alongside `MultiTouchInputMapper` on `virtio_input_multi_touch_1..11` for `SW_LID`).
   - `REL_WHEEL` + `.idc` `device.type = rotaryEncoder` → **`RotaryEncoderInputMapper`** (`AINPUT_SOURCE_ROTARY_ENCODER`; without the `.idc` property on API 29–34, a `REL_WHEEL`-only device gets `classes = 0` and is **completely ignored** by `EventHub`!).

#### 8.3.2 Master Android API-Level Input Routing Matrix (API 24 – API 36)

| API Level (Android) | Guest Kernel Input Drivers | Guest `.idc` / `.kl` / `.kcm` Files (`device/generic/goldfish`) | Multi-Display & Foldable Routing | `InputReader` Behavioral Regimes & Breaking Changes |
| :--- | :--- | :--- | :--- | :--- |
| **API 24–25**<br>(7.0–7.1 Nougat) | `3.10`: `CONFIG_KEYBOARD_GOLDFISH_EVENTS=y` (`qwerty2`). **No `CONFIG_VIRTIO_INPUT`!** | `/system/usr/idc/qwerty2.idc`, `/system/usr/keylayout/qwerty.kl`, `/system/usr/keychars/qwerty2.kcm`. | Single display only (`touch.displayId` does not exist yet). | Monolithic `InputReader.cpp`. Pure MT Protocol B without `BTN_TOUCH` works; `ABS_MT_PRESSURE == 0` → `HOVER_MOVE`; `MT_TOOL_MAX + BTN_TOOL_RUBBER` → `ERASER`. **NO `MT_TOOL_PALM`** (treated as `FINGER` → unintended tap!). **NO `ABS_TILT_X/Y` on MT**. |
| **API 26–28**<br>(8.0–9 Pie) | `3.18` / `4.4` / `4.9`: `goldfish_events` (`qwerty2`) for touch, keys, trackball, and `EV_SW` (`SW_LID`, `SW_HEADPHONE_INSERT`, `SW_MICROPHONE_INSERT`). **No `VirtioInput`.** | Same `/system/usr/` `qwerty2.idc` + `qwerty.kl`. | `touch.displayId` added in `TouchInputMapper` (API 26), unused on single-display Goldfish. | **API 26:** Added `View.requestPointerCapture()` (`SOURCE_MOUSE_RELATIVE` — works **only** on `CursorInputMapper`; actively **disables** `TouchInputMapper` `POINTER` devices such as `VirtioTablet`!). |
| **API 29**<br>(10 Q) | `4.14`: `goldfish_events` + `goldfish_rotary`; late-2019 Q SDK refresh backported `CONFIG_VIRTIO_INPUT=y` + `VirtioInput = on`. | Added `/vendor/usr/idc/goldfish_rotary.idc` (`b/33292948`); Q refresh added `virtio_input_multi_touch_1..11.idc`. | Foldables use `EV_SW SW_LID` (`PhoneWindowManager`) + `wm folded-area`. Static `input-port-associations.xml` introduced in framework (unused by Goldfish). | Still **NO `MT_TOOL_PALM`** — sending `MT_TOOL_PALM` then `TRACKING_ID = -1` fires a normal finger **`ACTION_UP` (unintended click)**! |
| **API 30**<br>(11 R) | `5.4`: `VirtioInput = on` standard (`CONFIG_VIRTIO_INPUT=y` + `CONFIG_KEYBOARD_GOLDFISH_EVENTS=y` retained for `-feature -VirtioInput`). | `/vendor/usr/idc/virtio_input_multi_touch_1..11.idc`; `/vendor/usr/idc/qwerty2.idc` (`keyboard.orientationAware = 0`, `keyboard.builtIn = 1`, `b/140563470`); `/vendor/usr/keylayout/qwerty.kl`. **No `QEMU_Virtio_Keyboard.idc`!** | `virtio_input_multi_touch_2..11.idc` bind `touch.displayId = virtual:com.android.emulator.multidisplay:1234562..1234571` (`MultiDisplayService`). Foldables still use `SW_LID` / folded-area. | `InputReader` split into `reader/mapper/*`. **Added `MT_TOOL_PALM`**, but implemented as **whole-gesture `cancelTouch()`** — palm on 1 slot aborts **all** active pointers across the screen! |
| **API 31–32**<br>(12 S / 12L Sv2) | GKI `5.10`: `virtio_input.ko` in `initramfs`; **zero** built-in `goldfish_events` or `goldfish_rotary` in GKI `vmlinux`. | Same `/vendor/usr/idc/virtio_input_multi_touch_1..11.idc`, `qwerty2.idc`, & `goldfish_rotary.idc` *(rotary `.idc` was not renamed to `virtio_input_rotary.idc` until API 33!)*. **No `QEMU_Virtio_Keyboard.idc`!** | **API 32 (12L):** `DeviceStateOnBoot = on` (`device_state_configuration.xml` + `android.sensor.hinge_angle`) replaces `SW_LID` for standard foldables (`lid` subtype still uses `SW_LID`). `display_settings.xml` switches to `imePolicy="0"`. | **API 31:** Upgraded `MT_TOOL_PALM` to **per-pointer cancellation** (`canceledIdBits` → `ACTION_POINTER_UP \| FLAG_CANCELED`, or `ACTION_CANCEL` if last pointer). Added `InputManager.addUniqueIdAssociation()`. |
| **API 33**<br>(13 T) | GKI `5.15`: `virtio_input.ko`. | Renamed `goldfish_rotary.idc` → `/vendor/usr/idc/virtio_input_rotary.idc` (`b/204794759`). **No `QEMU_Virtio_Keyboard.idc`!** | Same as API 32; last API requiring `qemu.hw.mainkeys` boot prop handling in legacy form. | Added `UnwantedInteractionBlocker`. **Note (API 24–33):** `MultiTouchInputMapper::hasStylus()` checks **only** `EV_KEY` (`BTN_TOOL_PEN` / `BTN_TOOL_RUBBER`) at configure time (`virtio-input-android` satisfies this via `BTN_TOOL_RUBBER`). |
| **API 34**<br>(14 U) | GKI `6.1`: `virtio_input.ko`. | **QPR (`b/289373564`):** `virtio_input_multi_touch_7.idc` (`head=6`) repurposed to `local:4619827551948147201` (`pixel_fold` outer display)! **No `QEMU_Virtio_Keyboard.idc`!** | `pixel_fold` dual physical displays driven by `hinge_angle` + `display_layout_configuration.xml` + `virtio_input_multi_touch_7.idc`. | Added `mStylusMtToolSeen` (dynamic `SOURCE_STYLUS`) and `TouchpadInputMapper` (`INPUT_PROP_POINTER`). **BREAKING (`b/309667960`):** `DeviceMode::POINTER` (`VirtioTablet`) changed to uniform `std::max(xScale, yScale)` aspect-ratio scaling! |
| **API 35**<br>(15 V) | GKI `6.6`: `virtio_input.ko`. | **BREAKING (`b/212639052`, `b/319814548`):** AOSP deleted `/system/usr/{idc,keylayout,keychars}/qwerty*`! Goldfish added `/vendor/usr/{keylayout/qwerty2.kl, keychars/qwerty2.kcm}`. **Still no `QEMU_Virtio_Keyboard.idc`!** | Same (`virtio_input_multi_touch_7.idc` reserved for `pixel_fold` outer screen). | Added `addUniqueIdAssociationByPort()` / `ByDescriptor()` and `vd_flags::virtual_rotary()` fallback for `.idc`-less `REL_WHEEL` devices. |
| **API 36**<br>(16 Baklava / `main`) | GKI `6.6` / `6.12`: `virtio_input.ko`. | **Added `/vendor/usr/idc/QEMU_Virtio_Keyboard.idc`** (`b/454612599`, mapping `"QEMU Virtio Keyboard"` → `qwerty2.kl`/`qwerty2.kcm`), Desktop `*_generic.idc`, and `touch.enableForInactiveViewport = 1` + `touch.wake = 1` in `virtio_input_multi_touch_*.idc` (`b/513696491`). | Desktop Mode (`DesktopModeWindowDecorViewModel` / `DragResizeInputListener`) requires `SOURCE_MOUSE` hover & right-click; Desktop Overview uses `KEY_SCALE (120)`. | `MultiTouchInputMapper` **still** hardcodes `tiltX = 0; tiltY = 0` (`ABS_TILT_X/Y` ignored on MT devices). Screen-off / AOD tap-to-wake enabled via `.idc` (`b/513696491`). |

#### 8.3.3 Feature & Form-Factor Routing Matrix

| Form Factor / Feature | `qemu2` Active Input Devices & Flags | Guest Android Mapper & `InputDevice` Source | Current `qemu-next` Behavior |
| :--- | :--- | :--- | :--- |
| **Phone / Tablet (API 30–36)** | `VirtioInput`: `virtio_input_multi_touch_1..11` + `virtio-keyboard-pci` (`"qwerty2"` if `hw.keyboard=yes`, else `goldfish-events` for hw buttons) | `MultiTouchInputMapper` (`SOURCE_TOUCHSCREEN \| SOURCE_STYLUS`) + `KeyboardInputMapper` | Touch, stylus/eraser, and basic typing work; 3D stylus tilt (`AXIS_TILT`), `KEY_CAMERA`, `KEY_SEARCH`, `KEY_MICMUTE`, DOM `"Home"`/`"End"`, and `hw.keyboard=no` soft-IME behavior regress; API 30–35 misses `qwerty2.idc`. |
| **Phone / Tablet (API 24–28 & early 29)** | `!VirtioInput`: `goldfish-events` (`"qwerty2"`, `have-multitouch=true`, `have-keyboard`, `have-dpad`, `have-trackball`, `EV_SW`) | `MultiTouchInputMapper` + `KeyboardInputMapper` + `SwitchInputMapper` on `"qwerty2"` | **Complete input failure:** guest kernel has no `virtio_input` driver; zero touchscreens and (on ARM64) zero keyboards exist. |
| **Foldables (API 29–31 vs. API 32–36)** | API 29–31: `SW_LID` on `virtio_input_multi_touch_1` + `folded-area`.<br>API 32–36: `hinge_angle` sensor + host cover-display remap (`virtio_input_multi_touch_7` on `pixel_fold`). | `SwitchInputMapper` (`SW_LID` → `PhoneWindowManager`) on API 29–31; `DeviceStateManager` + `MultiTouchInputMapper` on API 32+ | API 32+ `hinge_angle` foldables work (watch `head=6` / `virtio_input_multi_touch_7.idc` collision if >= 6 virtual displays); **API 29–31 lid foldables fail** because `SW_LID` is never emitted. |
| **Desktop / PC (`AVD_DESKTOP`, Freeform)** | `VirtioInput + VirtioMouse` (or `VirtioTablet`): `virtio-mouse-pci` / `virtio-tablet-pci` (**no** `virtio_input_multi_touch_*`), `KEY_SCALE (120)` for Overview on API 36+ | `CursorInputMapper` (`SOURCE_MOUSE` + `SOURCE_MOUSE_RELATIVE`) or `SingleTouchInputMapper` (`DeviceMode::POINTER`) | **Severely degraded:** no hover (`ACTION_HOVER_*`), no window-edge resize cursors, no maximize hover menu, no right/middle click, no mouse wheel scroll, no `requestPointerCapture()`, and `KEY_SCALE (120)` Overview is dropped. |
| **Wear OS (`AVD_WEAR`)** | `virtio_input_multi_touch_1` + `virtio_input_rotary_pci` (`API 30+`) / `goldfish_rotary` (`API <= 29`) + `KEY_STEM_PRIMARY/1/2/3 (264..267)` + palm `KEY_SLEEP (142)` | `MultiTouchInputMapper` + `RotaryEncoderInputMapper` (`SOURCE_ROTARY_ENCODER`) + `KeyboardInputMapper` | Crown/bezel rotary (`WheelEvent` / `AxisEvent`) and hardware stem buttons (`264..267`) are completely dead. |
| **Android TV (`hw.screen="no-touch"`, `hw.dPad=yes`)** | **Zero** `virtio_input_multi_touch_*` devices; D-pad (`UP/DOWN/LEFT/RIGHT` + `LINUX_KEY_CENTER = 352`) via `virtio-keyboard-pci` / `goldfish-events` | `KeyboardInputMapper` (`SOURCE_KEYBOARD \| SOURCE_DPAD`) | Unconditionally attaches 11 touchscreens despite `hw.screen="no-touch"`, and `QemuKeyboardSink` drops `LINUX_KEY_CENTER (352)` (`DPAD_CENTER` via `KEY_OK`). |
| **Automotive (`AVD_ANDROID_AUTO`)** | `virtio_input_multi_touch_1..11` (cluster + center displays) + optional `virtio_input_rotary_pci` (`hw.rotaryInput=yes`) / `CAR_ROTARY` | `MultiTouchInputMapper` (per display via `.idc`) + `RotaryEncoderInputMapper` | Multi-display touch works (except slot 6 `virtio_input_multi_touch_7.idc`); rotary controller input is dead. |
| **Android XR (`AVD_XR`)** | `VirtioMouse + VirtioDualModeMouse` (`virtio-dual-mode-mouse-pci`, **no** `virtio_input_multi_touch_*`) + QEMUD `"xr_service"` pipe | `CursorInputMapper` + `SingleTouchInputMapper` (`POINTER`) + guest XR runtime reading `"xr_service"` | 6DoF head pose, hand raycast, eye tracking, recenter, and dual-mode cursor/grab are completely missing. |
| **AI Glasses (`AVD_GLASSES`)** | `hw.screen="no-touch"`, `hw.touchpad0=yes` (`1543x297` `virtio_input_multi_touch_touchpad_pci_0`), `KEY_STEM_1 (265)` & `KEY_STEM_2 (266)` | Temple touchpad (`MultiTouchInputMapper` / `TouchpadInputMapper`) + stem keys | Touchpad (`TouchpadEvent`) and `KEY_STEM_1/2` buttons are completely missing; 11 unintended touchscreens are attached instead. |

#### 8.3.4 Seven Subtle Android `InputReader` Traps When Porting Older API Levels

1. **Trap 1 — Two-Frame `MT_TOOL_PALM` Synthesizes Unintended Clicks on API 24–29 (and Aborts All Fingers on API 30):**
   - In Android 7.0–10 (`API 24–29`), `Slot::getToolType()` only recognizes `MT_TOOL_FINGER (0)` and `MT_TOOL_PEN (1)`. Sending `ABS_MT_TOOL_TYPE = MT_TOOL_PALM (2)` falls through to `ToolType::UNKNOWN` → `TOOL_TYPE_FINGER`, so §4.2's two-frame cancel dispatches `ACTION_MOVE` followed by **`ACTION_UP` — clicking whatever UI widget was under the cancelled finger** (including on 60 s unary idle expiry or client disconnect!).
   - In Android 11 (`API 30`), `MT_TOOL_PALM` invokes `cancelTouch()`, aborting **every** active finger across all sessions on that display (`mCurrentMotionAborted = true`). Only **API 31+** supports per-slot `canceledIdBits`.

2. **Trap 2 — Keyboard `ID_NAME` (`"QEMU Virtio Keyboard"` vs. `"qwerty2"`) & `102 = HOME` vs. `MOVE_HOME`:**
   - `/vendor/usr/idc/QEMU_Virtio_Keyboard.idc` was **only added in API 36** (`b/454612599`). On **API 30–35**, `"QEMU Virtio Keyboard"` (`0x0627:0x0001`) matches **no `.idc` file** and falls back to `/system/usr/keylayout/Generic.kl` with `keyboard.orientationAware` unset (`0` by default for non-touch keyboards, but missing `qwerty.kl` / `qwerty2.kl` custom Android mappings `264..267 = STEM_*`, `452 = MOVE_HOME`, `453 = MOVE_END`).
   - Conversely, on **API 36** (where `QEMU_Virtio_Keyboard.idc` loads `qwerty2.kl`) or when named `"qwerty2"` (`API 24–36`), `qwerty.kl` / `qwerty2.kl` maps evdev **`102` (`KEY_HOME`) to Android `HOME` (launcher home screen!)** and expects **`452` (`0x1c4`) for cursor `MOVE_HOME`** and **`453` (`0x1c5`) for `MOVE_END`**. Because `QemuKeyboardSink` drops `452`/`453` (`Q_KEY_CODE_UNMAPPED`) and maps `Q_KEY_CODE_HOME` to `102`, pressing the physical `Home` key either does nothing (DOM `"Home"` → `452` dropped) or exits the app to the home screen (`evdev 102` → `HOME`)!
   - Also note **API 35 (`b/212639052`)**: AOSP deleted `/system/usr/{idc,keylayout,keychars}/qwerty*`; API 35+ images only carry `/vendor/usr/{idc/qwerty2.idc, keylayout/qwerty2.kl, keychars/qwerty2.kcm}`.

3. **Trap 3 — Accidental Zero Pressure Turns Taps into Hover (`API 24–36`):**
   - From API 24 through API 36, `MultiTouchInputMapper::syncTouch()` computes:
     ```cpp
     bool isHovering = mTouchButtonAccumulator.getToolType() != ToolType::MOUSE &&
             (mTouchButtonAccumulator.isHovering() ||
              (mRawPointerAxes.pressure && inSlot.getPressure() <= 0));
     ```
   - Because `virtio-input-android` advertises `ABS_MT_PRESSURE`, any active slot (`ABS_MT_TRACKING_ID >= 0`) with `ABS_MT_PRESSURE == 0` is dispatched as **`ACTION_HOVER_ENTER` / `ACTION_HOVER_MOVE`**, never `ACTION_DOWN` (`ABS_MT_DISTANCE` is never checked by `isHovering`).
   - Likewise, on contact lift, `ABS_MT_PRESSURE = 0` and `ABS_MT_TRACKING_ID = -1` **must** stay in the **same `SYN_REPORT` frame**; splitting them across two frames causes a spurious `ACTION_UP → HOVER_ENTER → HOVER_MOVE → HOVER_EXIT` burst on every tap release.

4. **Trap 4 — `MultiTouchInputMapper` Ignores `ABS_TILT_X` and `ABS_TILT_Y` (`API 24–36`), & API 24–33 `hasStylus()` Gate:**
   - Even in Android 16 (`API 36`), `MultiTouchInputMapper::configureRawPointerAxes()` never reads `ABS_TILT_X`/`ABS_TILT_Y` and `syncTouch()` hardcodes `outPointer.tiltX = 0; outPointer.tiltY = 0`. Only **`SingleTouchInputMapper`** (`ABS_X/Y + BTN_TOUCH + BTN_TOOL_PEN`) populates `MotionEvent.AXIS_TILT` and full `[-π, π]` stylus `AXIS_ORIENTATION`.
   - On **API 24–33** (prior to API 34's `mStylusMtToolSeen`), `MultiTouchInputMapper::hasStylus()` only checks `EV_KEY` (`BTN_TOOL_PEN` or `BTN_TOOL_RUBBER`). Keeping `BTN_TOOL_RUBBER` (and ideally `BTN_TOOL_PEN`) in `EV_KEY` is mandatory so `InputDevice.supportsSource(SOURCE_STYLUS)` is `true` from boot.

5. **Trap 5 — API 34+ `VirtioTablet` Uniform Aspect-Ratio Scaling (`b/309667960`):**
   - Starting in **Android 14 (`API 34`)**, `TouchInputMapper::computeInputTransforms()` changed `DeviceMode::POINTER` from independent `(xScale, yScale)` to uniform `scale = std::max(xScale, yScale)`. On a `1080x2400` portrait panel, scaling `ABS_X` across `[0..32767] → [0..1080]` causes guest X coordinates to overshoot by `2400 / 1080 = 2.22x` unless the host scales both X and Y against `max(width, height)` (`qemu-user-event-agent-impl.c:141-150`).

6. **Trap 6 — `View.requestPointerCapture()` Disables `VirtioTablet` (`API 26+`):**
   - In `TouchInputMapper.cpp:933`, `DeviceMode::POINTER` checks `!mConfig.pointerCaptureRequest.isEnable()`, setting `DeviceMode::DISABLED` whenever an app requests pointer capture. Only `CursorInputMapper` (`REL_X/Y + BTN_MOUSE`)—or `TouchpadInputMapper` on API 34+—can enter `POINTER_RELATIVE` (`SOURCE_MOUSE_RELATIVE`).

7. **Trap 7 — Display Rotation, Missing `INPUT_PROP_DIRECT`, & Pre-API-36 Screen-Off Touch:**
   - Across **all API levels 24–36**, when `touch.orientationAware = 1` (or `INPUT_PROP_DIRECT` is advertised), `TouchInputMapper` outputs natural (`ROTATION_0`) panel coordinates and `InputDispatcher` applies display rotation per window—confirming §7's natural-panel-coordinate invariant holds down to API 24.
   - However, because `virtio-input-android` omits `VIRTIO_INPUT_CFG_PROP_BITS = (1 << INPUT_PROP_DIRECT)`, any image missing a matching `.idc` file falls back to `DeviceType::POINTER` (`orientationAware = false`), turning touches into rotated relative mouse gestures.
   - Prior to **API 36** (`b/513696491`), `virtio_input_multi_touch_*.idc` lacked `touch.enableForInactiveViewport = 1` and `touch.wake = 1`, so `TouchInputMapper` drops touches while the display is off/dozing; waking older APIs requires `KEY_POWER` / `KEY_WAKEUP`.

---

### 8.4 Roadmap for Porting Older API Levels & Closing Remaining Gaps

All remaining guest-device and API-level gaps above can be closed **without** changing the core architecture in §§2–3 (single QEMU-loop `InputDispatcher` owner, policy bound at construction, and complete `StreamInputActivity` observation):

1. **Bind API-Level Touch Encoding Policy at Construction (`TouchEncodingOptions`):**
   - Add `VIRTIO_INPUT_CFG_PROP_BITS` (`1 << INPUT_PROP_DIRECT`) and `BTN_TOOL_PEN` (alongside `BTN_TOOL_RUBBER` and `BTN_STYLUS`) in `virtio_input_android_config.c`.
   - Parameterize `AppendContactCancel` by `api_level` at construction:
     - **`api_level >= 31`:** Keep the two-frame `MT_TOOL_PALM` → `ABS_MT_TRACKING_ID = -1` per-pointer cancel (§4.2).
     - **`api_level <= 30`:** Do **not** emit `MT_TOOL_PALM` on **API 24–29** (where it causes an unintended tap!). Either lift directly (or translate off-screen before lift) or, on **API 30**, emit `MT_TOOL_PALM` only when cancelling *all* contacts on the display so one agent session does not abort a concurrent human gesture.
   - Avoid routing non-`pixel_fold` virtual display #6 to `virtio_input_multi_touch_7` (`head=6`) on **API 34 QPR–36**, since guest `virtio_input_multi_touch_7.idc` is reserved for `local:4619827551948147201` (`b/289373564`).

2. **Introduce a Direct Evdev `KeyboardSink` & Conditional `"qwerty2"` Naming:**
   - Replace (or extend) `virtio-keyboard-pci` + `QemuKeyboardSink` so resolved 10-bit Linux `evdev` keycodes (`KEY_SCALE`, `KEY_OK/CENTER`, `ANDROID_KEY_STEM_*`, `KEY_CAMERA`, `KEY_SEARCH`, `KEY_MICMUTE`, `KEY_SEND`, `KEY_ALL_APPS`, and `0x1c4`/`0x1c5` `MOVE_HOME`/`MOVE_END`) are written directly to an Android VirtIO keyboard/button device advertising `mandatory_android_keys`—eliminating the lossy `QKeyCode` bottleneck.
   - On **API 30–35**, advertise `VIRTIO_INPUT_CFG_ID_NAME = "qwerty2"` (matching `/vendor/usr/idc/qwerty2.idc` + `qwerty.kl`/`qwerty2.kl`); on **API 36+**, either `"qwerty2"` or `"QEMU Virtio Keyboard"` works (`b/454612599`). Ensure DOM `"Home"`/`"End"` emit `452`/`453` when backed by `qwerty*.kl` and `102`/`107` when backed by `Generic.kl`.
   - When `hw.keyboard = no`, expose only a non-alphabetic hardware-button device (`KEY_POWER`, `KEY_VOLUME*`, `KEY_HOME`, `KEY_BACK`, `KEY_APPSWITCH`, `KEY_SLEEP`, `KEY_STEM_*`, D-pad) so Android does not suppress the soft IME, and pass `qemu.hw.mainkeys` on **API <= 33**.

3. **Add a `GoldfishEventsSink` (`GFSH0002`, `"qwerty2"`) Behind `TouchDevices` & `KeyboardSink` for API 24–29:**
   - When `api_level <= 28` (or `!VirtioInput` on API 29), realize the SysBus `goldfish-events` device (`GFSH0002` / FDT `google,goldfish-events-keypad`) and bind Display 0's `TouchDeviceSink`, `KeyboardSink`, and `SwitchSink` to it.
   - Because `goldfish-events` natively speaks Linux Multi-Touch Protocol B (`ABS_MT_SLOT` `0..10`), `EV_KEY`, and `EV_SW` (`SW_LID`, `SW_HEADPHONE_INSERT`, `SW_MICROPHONE_INSERT`), **`InputDispatcher`, `ContactTable`, and `touch_encoding` require zero architectural changes above the sink interface!**

4. **Add Optional Pointer (`MouseSink`), Rotary (`AxisSink`), Touchpad, Switch, & XR Bridges Under `InputDispatcher`:**
   - **Desktop / XR / Pointer Capture (`VirtioMouse` / `VirtioDualModeMouse`):** When `AVD_DESKTOP`, `AVD_XR`, `hw.screen == "no-touch"`, or `VirtioDualModeMouse` is active, instantiate `virtio-dual-mode-mouse-pci` (or `virtio-mouse-pci` + `virtio-tablet-pci` with `max(w, h)` scaling on `api_level >= 34`, `b/309667960`) and select a `DualModePointerTranslator` at construction instead of `SingleFingerMouseTranslator`, forwarding `REL_X/Y`, `ABS_X/Y`, `BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA`, and `REL_WHEEL`/`REL_HWHEEL` through `InputDispatcher`.
   - **Rotary (`hw.rotaryInput` / `AVD_WEAR`):** Realize `virtio_input_rotary_pci` (`API >= 30`, matching `virtio_input_rotary.idc` on API 33+ or `goldfish_rotary.idc` if named `"goldfish_rotary"` on API 29–32!) or `goldfish_rotary` (`API <= 29`), and wire `AxisEvent` (`AXIS_TYPE_ROTARY_ENCODER`) + v1 `WheelEvent` through `InputDispatcher`.
   - **Stylus 3D Tilt (Optional v2 Capability):** If non-zero `MotionEvent.AXIS_TILT` is required by v2 clients, expose a dedicated single-touch `virtio-input` stylus device (`SingleTouchInputMapper`: `ABS_X/Y`, `ABS_PRESSURE`, `ABS_DISTANCE`, `ABS_TILT_X/Y`, `BTN_TOUCH`, `BTN_TOOL_PEN`, `BTN_TOOL_RUBBER`, `BTN_STYLUS`, `INPUT_PROP_DIRECT`).
   - **Switches & Lid Foldables:** Emit `{EV_SW, SW_LID, folded ? 1 : 0}` on `virtio_input_multi_touch_1` / `goldfish-events` from `FoldableModel` when `api_level <= 31` or `hinge.sub_type == lid`, and route raw `AndroidEvent` (`EV_KEY` / `EV_SW`) to the sink that advertises the target capability bit.
   - **Glasses Touchpad & XR Service:** Conditionally realize `virtio_input_multi_touch_touchpad_pci_0` when `hw.touchpad0 = yes` (`AVD_GLASSES`), omit `virtio-input-android` touchscreens when `hw.screen = "no-touch"`, and bridge `Xr*Event` messages to the `"xr_service"` QEMUD/vsock service on `AVD_XR`.

5. **Expand Telnet `LegacyConsoleBridge` Event Support:**
   - Update `LegacyConsoleBridge` (`emulator/telnet`) to support the full Linux `EV_KEY`, `EV_ABS`, `EV_REL`, `EV_SW`, and `EV_SYN` symbol tables in `event send` / `event types` / `event codes` and forward optional display IDs in `event mouse`.
