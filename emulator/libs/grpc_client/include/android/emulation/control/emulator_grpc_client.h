// Copyright (C) 2025 The Android Open Source Project
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
#pragma once

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "grpc_endpoint_description.pb.h"

#include "goldfish/discovery/emulator_advertisement.h"
#include "goldfish/eventing/event_sources.h"

namespace android::emulation::control {

// Forward declarations
class BlockingEmulatorGrpcClient;
class CallbackEmulatorGrpcClient;
class EmulatorGrpcClientBuilder;

using ::android::emulation::remote::Endpoint;
using grpc::experimental::ClientInterceptorFactoryInterface;
using InterceptorFactory = std::unique_ptr<ClientInterceptorFactoryInterface>;
using InterceptorFactories = std::vector<InterceptorFactory>;
using ::goldfish::discovery::EmulatorAdvertisement;
using ::goldfish::discovery::EmulatorProperties;

/**
 * @brief Represents the current state of the gRPC connection.
 */
enum class ConnectionState : std::uint8_t {
    kDisconnected,  ///< The client is not connected.
    kConnecting,    ///< A connection attempt is in progress.
    kConnected      ///< The client is connected and ready for RPC calls.
};

// PIMPL class, defined in the .cpp file.
class EmulatorGrpcClientImpl;

/**
 * @brief A non-public base class for gRPC clients.
 *
 * This class owns the implementation (PIMPL) and provides the common,
 * non-virtual API shared by both the blocking and callback-based clients.
 */
class EmulatorGrpcClientBase {
  public:
    virtual ~EmulatorGrpcClientBase();

    /**
     * @brief Gets the endpoint configuration for this client.
     * @return A const reference to the `Endpoint` object.
     */
    const Endpoint& GetEndpoint() const;

    /**
     * @brief Gets the current connection state.
     * @return The current `ConnectionState`.
     */
    ConnectionState GetConnectionState() const;

    /**
     * @brief Creates a new gRPC stub for a specific service.
     *
     * This template method provides a convenient way to get a typed stub for
     * making RPC calls. The client must be in the `Connected` state before
     * calling this method.
     *
     * @tparam T The gRPC service type (e.g., `EmulatorController::Service`).
     * @return A `StatusOr` containing either a unique pointer to the stub or an
     *         error if the client is not connected.
     */
    template <class T>
    absl::StatusOr<std::unique_ptr<typename T::Stub>> Stub() {
        if (GetConnectionState() != ConnectionState::kConnected) {
            return absl::FailedPreconditionError(
                    absl::StrCat("gRPC client for '", GetEndpoint().target(),
                                 "' is not connected. Call Connect() or ConnectAsync() "
                                 "before creating a service stub."));
        }
        return T::NewStub(GetChannel());
    }

    /**
     * @brief Creates a new `ClientContext` for an RPC call.
     *
     * This method pre-configures the context with any necessary security
     * details (like authentication tokens) derived from the `Endpoint`
     * configuration.
     *
     * @return A `StatusOr` containing either the context or an error.
     */
    absl::StatusOr<std::unique_ptr<grpc::ClientContext>> NewContext();

  protected:
    EmulatorGrpcClientBase();
    friend class EmulatorGrpcClientBuilder;

    std::shared_ptr<::grpc::Channel> GetChannel();
    std::shared_ptr<EmulatorGrpcClientImpl> p_impl_;
};

/**
 * @brief A simple, synchronous gRPC client for the emulator.
 *
 * This client provides a straightforward, blocking API for connecting and
 * making RPC calls. It is ideal for simple, short-lived tasks or scripts.
 *
 * @par Trade-offs
 * This client is lightweight and creates no background threads. However, it
 * does not perform automatic liveness monitoring. If the connection is dropped
 * by the server, this client will not be aware of it until the next RPC call
 * fails.
 *
 * @par Example
 * @code
 *   // Note: In a real application, you would need to include the specific
 *   // service headers (e.g., "emulator_controller.grpc.pb.h").
 *
 *   auto builder = EmulatorGrpcClientBuilder()
 *                      .WithDiscoveryFile("path/to/discovery.ini");
 *
 *   auto clientOrStatus = builder.BuildBlocking();
 *   if (!clientOrStatus.ok()) { // handle config error/ }
 *   auto client = std::move(*clientOrStatus);
 *
 *   absl::Status status = client->Connect(absl::Seconds(5));
 *   if (!status.ok()) { // handle connection error  }
 *
 *   // ... make RPC calls ...
 *
 *   client->Disconnect();
 * @endcode
 */
class BlockingEmulatorGrpcClient : public EmulatorGrpcClientBase {
  public:
    /**
     * @brief Establishes a synchronous connection to the gRPC server.
     *
     * This method blocks until the connection is established or the timeout
     * expires.
     *
     * @param timeout The maximum time to wait for a connection.
     * @return `absl::OkStatus()` on success, or an error status on failure.
     */
    absl::Status Connect(absl::Duration timeout);

    /**
     * @brief Closes the gRPC connection.
     */
    void Disconnect();
};

/**
 * @brief An advanced, asynchronous gRPC client for the emulator.
 *
 * This client provides a non-blocking, callback-based API for connecting and
 * making RPC calls. It is designed for long-lived applications, such as user
 * interfaces, that need to remain responsive and aware of the connection status.
 *
 * @par Trade-offs
 * This client provides significant benefits, including a non-blocking `ConnectAsync`
 * method, a `ConnectionStateChanges` event source for reactive UI updates, and
 * automatic liveness monitoring to detect dropped connections. This is achieved
 * by managing a background worker thread, which introduces a small amount of
 * resource overhead compared to the `BlockingEmulatorGrpcClient`.
 *
 * @par Example
 * @code
 *   auto builder = EmulatorGrpcClientBuilder()
 *                      .WithEndpoint(endpoint);
 *
 *   auto clientOrStatus = builder.BuildCallback();
 *   if (!clientOrStatus.ok()) { // handle config error  }
 *   auto client = std::move(*clientOrStatus);
 *
 *   // Subscribe to state changes to update a UI element.
 *   auto handle = android::base::eventing::makeScopedCallback(
 *       client->ConnectionStateChanges(),
 *       [](ConnectionState state) {
 *           // Update UI based on state: Connecting, Connected, Disconnected
 *       });
 *
 *   // Initiate a non-blocking connection.
 *   std::future<absl::Status> future = client->ConnectAsync(absl::Seconds(10));
 *   future.wait(); // Or handle the result on another thread.
 *
 *   // ... application logic ...
 * @endcode
 */
class CallbackEmulatorGrpcClient : public EmulatorGrpcClientBase {
  public:
    /**
     * @brief Initiates a non-blocking connection attempt.
     *
     * This method returns immediately and performs the connection attempt in the
     * background.
     *
     * @param timeout The maximum time to wait for a connection.
     * @return A `std::future` that will be fulfilled with the connection status
     *         (`absl::OkStatus()` on success).
     */
    std::future<absl::Status> ConnectAsync(absl::Duration timeout);

    /**
     * @brief Closes the gRPC connection and stops the monitoring thread.
     */
    void Disconnect();

    /**
     * @brief Gets an event source for monitoring connection state changes.
     *
     * Applications can subscribe to this event source to receive notifications
     * when the client connects, disconnects, or starts a connection attempt.
     *
     * @return A reference to the `CallbackEventSource`.
     */
    android::base::eventing::CallbackEventSource<ConnectionState>& ConnectionStateChanges();
};

/**
 * @brief A builder for constructing all types of EmulatorGrpcClient instances.
 *
 * This factory is the single entry point for creating either a simple
 * `BlockingEmulatorGrpcClient` or an advanced `CallbackEmulatorGrpcClient`.
 */
class EmulatorGrpcClientBuilder {
  public:
    /**
     * @brief Configures the client using an emulator discovery file.
     *
     * The discovery file is a properties file (key-value pairs) that the
     * emulator writes at startup. It provides a flexible way to configure the
     * gRPC connection details without hardcoding them.
     *
     * The builder parses this file to determine the server address and security
     * settings. The `grpc.port` property is used to construct the target
     * address (e.g., "localhost:<port>"). If a `grpc.token` is present, it
     * will be automatically added as a `Bearer` token to the `authorization`
     * header for every RPC call.
     *
     * @param discovery_file The path to the discovery file.
     * @return A reference to the builder for chaining.
     *
     * @par Example Discovery File
     * @code
     *   # discovery.properties
     *   grpc.port=8554
     *   grpc.token=abcdef12345
     *   ...
     * @endcode
     */
    EmulatorGrpcClientBuilder& WithDiscoveryFile(const std::filesystem::path& discovery_file);

    /**
     * @brief Configures the client to connect to an emulator matching specific properties.
     *
     * This method searches the discovery directory for an active emulator that
     * matches the provided properties. It uses the provided `EmulatorAdvertisement`
     * instance to perform the discovery.
     *
     * The method will use the first discovered emulator that contains all the
     * key-value pairs specified in @p properties. Once a matching emulator is found,
     * its discovery file is parsed to extract the gRPC port (`grpc.port`) and
     * optional security token (`grpc.token`) to configure the connection endpoint.
     *
     * If no matching emulator is found, or if the discovery file is malformed,
     * the builder's error status is set, and subsequent calls to `BuildBlocking()`
     * or `BuildCallback()` will fail.
     *
     * @param properties The key-value pairs that the emulator must match.
     * @param advertisement The advertisement instance used to scan for running emulators.
     *                      Defaults to a default-constructed instance targeting the
     *                      standard discovery directory.
     * @return A reference to the builder for chaining.
     */
    EmulatorGrpcClientBuilder& ForDiscoveredEmulator(
            EmulatorProperties properties,
            const EmulatorAdvertisement& advertisement = EmulatorAdvertisement());

    /**
     * @brief Configures the client using a pre-constructed `Endpoint` object.
     *
     * This method allows for programmatic configuration of the gRPC connection.
     * The `Endpoint` message defines the server's address, security credentials,
     * and any custom metadata headers that should be sent with every RPC call.
     *
     * @param endpoint The `Endpoint` object with the desired configuration.
     * @return A reference to the builder for chaining.
     *
     * @par Example: Using a JWT Token
     * @code
     *   Endpoint endpoint;
     *   endpoint.set_target("localhost:8554");
     *
     *   // Add a JWT token for authentication.
     *   auto* security = endpoint.mutable_security();
     *   auto& headers = *security->mutable_metadata_headers();
     *   headers["authorization"] = "Bearer your-jwt-token-here";
     *
     *   // You can also configure TLS from a file.
     *   // security->set_server_ca_certificate("path/to/ca.pem");
     *
     *   auto client = EmulatorGrpcClientBuilder()
     *                     .WithEndpoint(endpoint)
     *                     .BuildBlocking();
     * @endcode
     */
    EmulatorGrpcClientBuilder& WithEndpoint(const Endpoint& endpoint);

    /**
     * @brief Configures the client from an explicit target address (with optional
     * bearer token), falling back to a discovery file or auto-discovered emulator.
     *
     * Resolution order:
     * 1. If @p target is non-empty, connects to @p target (attaching @p token as
     *    a Bearer authorization header when non-empty).
     * 2. Otherwise, if @p discovery_file is non-empty, loads @p discovery_file.
     * 3. Otherwise, discovers the first running emulator via `ForDiscoveredEmulator({})`.
     *
     * @param target Optional gRPC target address (e.g., "localhost:8554").
     * @param token Optional authentication token attached as a Bearer header.
     * @param discovery_file Optional path to an emulator discovery `.ini` file.
     * @return A reference to the builder for chaining.
     */
    EmulatorGrpcClientBuilder& WithTargetOrDiscovery(
            std::string_view target, std::string_view token = "",
            const std::filesystem::path& discovery_file = {});
    /**
     * @brief Adds a gRPC interceptor to the client.
     *
     * Interceptors can be used to observe or modify RPC calls. This is useful
     * for implementing cross-cutting concerns like logging, authentication, or
     * metrics collection. You can add multiple interceptors, and they will be
     * executed in the order they are added.
     *
     * @param factory A unique pointer to the interceptor factory.
     * @return A reference to the builder for chaining.
     *
     * @par Example: Adding a Logging Interceptor
     * @code
     *   #include "google/protobuf/android/control/interceptor/logging_interceptor.h"
     *
     *   // ...
     *
     *   auto client = EmulatorGrpcClientBuilder()
     *       .WithEndpoint(endpoint)
     *       .WithInterceptor(std::make_unique<
     *           android::control::interceptor::StdOutLoggingInterceptorFactory>())
     *       .BuildBlocking();
     * @endcode
     */
    EmulatorGrpcClientBuilder& WithInterceptor(
            std::unique_ptr<ClientInterceptorFactoryInterface> factory);

    /**
     * @brief Builds a `BlockingEmulatorGrpcClient`.
     *
     * This method constructs a synchronous client based on the configuration
     * provided to the builder. The returned client will be in the
     * `Disconnected` state. You must call `Connect()` before making any RPC
     * calls.
     *
     * @return A `StatusOr` containing either the client instance or an error
     *         if the configuration was invalid.
     */
    absl::StatusOr<std::unique_ptr<BlockingEmulatorGrpcClient>> BuildBlocking();

    /**
     * @brief Builds a `CallbackEmulatorGrpcClient`.
     *
     * This method constructs an asynchronous, callback-based client. The
     * returned client will be in the `Disconnected` state. You must call
     * `ConnectAsync()` to initiate a connection.
     *
     * @return A `StatusOr` containing either the client instance or an error
     *         if the configuration was invalid.
     */
    absl::StatusOr<std::unique_ptr<CallbackEmulatorGrpcClient>> BuildCallback();

    /**
     * @brief Builds a raw gRPC channel and returns the resolved target address.
     *
     * Useful for proxy servers (such as gRPC-Web or Aquarium UI) that forward
     * raw frames over a channel without wrapping it in a Blocking or Callback
     * client.
     *
     * @return A `StatusOr` containing the resolved target address and channel,
     *         or an error if endpoint resolution or channel creation failed.
     */
    absl::StatusOr<std::pair<std::string, std::shared_ptr<grpc::Channel>>> BuildTargetAndChannel();

  private:
    template <typename TClient>
    absl::StatusOr<std::unique_ptr<TClient>> BuildClient();

    absl::Status status_{absl::OkStatus()};
    InterceptorFactories factories_;
    Endpoint destination_;
};

}  // namespace android::emulation::control
