# Component: FPS Calculator

**Role:** Utility for calculating the moving average Frames Per Second (FPS) using a sliding window of timestamps.
**Location:** `hardware/generic/goldfish/emulator/libs/fps_calculator`
**Namespace:** `goldfish`

## Integration Guide
| Class / Interface | Bazel Target | Header Path | Description |
| :--- | :--- | :--- | :--- |
| `FpsCalculator` | `@goldfish//emulator/libs/fps_calculator` | `include/goldfish/fps_calculator.h` | Main class for tracking frame times and computing FPS. |

## Critical Infrastructure
| File | Responsibility |
| :--- | :--- |
| `fps_calculator.cc` | Implements the ring buffer logic for timestamp storage and FPS calculation. |

## Dependencies
* **Upstream:** `@abseil-cpp//absl/time`, `@abseil-cpp//absl/time:clock_interface` (Time primitives and clock interface).

## Threading Model
* **Not Thread Safe:** The `FpsCalculator` class is **not** thread-safe.
* **Usage Rule:** If `AddFrame` and `GetFps` are called from different threads (e.g., a Render Thread adding frames and a UI Thread querying FPS), the caller **MUST** provide external synchronization (e.g., a `std::mutex`).

## Logic & Algorithms
### Sliding Window Calculation
The calculator maintains a fixed-size ring buffer of timestamps.
*   **Formula:** `FPS = (N - 1) / (Time_Newest - Time_Oldest)`
*   **Edge Case:** Returns `0.0` if fewer than 2 frames are recorded or if the duration is zero.
