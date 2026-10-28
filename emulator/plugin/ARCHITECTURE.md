# Component: Emulator Plugins

**Role:** Extensions and Devices that plug into the QEMU emulator core to provide Android-specific functionality.
**Location:** `hardware/generic/goldfish/emulator/plugin`
**Namespace:** `goldfish::*`

## Plugin Index

### Infrastructure & Transport
| Plugin | Role | Documentation |
| :--- | :--- | :--- |
| **vsock_goldfish** | Virtio-Vsock backend (Host-Guest transport). | [Architecture](vsock_goldfish/ARCHITECTURE.md) |
| **netsim** | WiFi/BT/UWB simulation bridge to `netsimd`. | [Architecture](netsim/ARCHITECTURE.md) |
| **adb-vsock** | Bridges Guest `adbd` to Host via VSOCK. | [Architecture](adb-vsock/ARCHITECTURE.md) |
| **webrtc** | In-process WebRTC QEMU device plugin. | [Architecture](webrtc/ARCHITECTURE.md) |

### Virtual Hardware & HALs
| Plugin | Role | Documentation |
| :--- | :--- | :--- |
| **battery** | Emulated Battery (MMIO). | [Architecture](battery/ARCHITECTURE.md) |
| **virtio-input-android** | Multi-touch input device (Virtio). | [Architecture](virtio-input-android/ARCHITECTURE.md) |
| **virtio-wifi** | Emulated WiFi device (Virtio). | [Architecture](virtio-wifi/ARCHITECTURE.md) |
| **display** | Virtual display and Pixman framebuffer backend. | [Architecture](display/ARCHITECTURE.md) |
| **audio** | QEMU guest audio capture and WebRTC audio bridge. | [Architecture](audio/ARCHITECTURE.md) |
| **wasapi** | Windows WASAPI audio backend (`AudioMixengBackend`). | [Architecture](wasapi/ARCHITECTURE.md) |
| **hal** | Hardware Abstraction Layer devices (GPS, Sensors, Camera, Vehicle, Multi-Display, virtio-gpu-gfxstream). | [Architecture](hal/ARCHITECTURE.md) |

### Control & Management
| Plugin | Role | Documentation |
| :--- | :--- | :--- |
| **avd** | Bootstrap device (`avdstart`), HAL registration. | [Architecture](avd/ARCHITECTURE.md) |
| **avd_universe** | State model and sensor state repository for AVD. | [Architecture](avd_universe/ARCHITECTURE.md) |
| **grpc** | Emulator Controller gRPC service. | [Architecture](grpc/ARCHITECTURE.md) |
| **vminterface** | VM Control API (`VmOperations`). | [Architecture](vminterface/ARCHITECTURE.md) |
| **system** | System abstractions (Time). | [Architecture](system/ARCHITECTURE.md) |
