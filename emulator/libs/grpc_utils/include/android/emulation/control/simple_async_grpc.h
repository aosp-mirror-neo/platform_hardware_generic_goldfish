// Copyright (C) 2022 The Android Open Source Project
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
#include <grpcpp/support/client_callback.h>

#include <deque>
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/functional/any_invocable.h"
#include "absl/synchronization/mutex.h"

/**
 * @class WithReactorLock
 * @brief A common synchronization base class virtually inherited by both reader and writer mixins.
 *
 * @details `reactor_lock_` is protected to ensure strict encapsulation and prevent external lock
 * tampering. It provides a unified synchronization primitive for all critical reactor transitions
 * (`StartRead`, `StartWrite`, and `Finish`).
 */
class WithReactorLock {
  protected:
    mutable absl::Mutex reactor_lock_;
};

/**
 * @struct Select
 * @brief Selects a type from a pack of types based on the provided index.
 *
 * @tparam Index The index of the type to select.
 * @tparam Args The parameter pack of types.
 */
template <std::size_t Index, typename... Args>
struct Select {
    /**
     * @brief Calculates the index of the last available type in the pack.
     */
    static constexpr std::size_t LastIndex = sizeof...(Args) - 1;

    /**
     * @brief Selects the type at the specified index from the pack of types.
     * If the index is out of range, the last available type is selected.
     */
    using type = typename std::tuple_element<(Index < sizeof...(Args) ? Index : LastIndex),
                                             std::tuple<Args...>>::type;
};

/**
 * @struct Select<Index, Template<Args...>>
 * @brief Specialization for template types with a pack of template arguments.
 */
template <std::size_t Index, template <typename...> class Template, typename... Args>
struct Select<Index, Template<Args...>> {
    using type = typename Select<Index, Args...>::type;
};

/**
 * @brief Convenient type alias for the selected type.
 */
template <std::size_t Index, typename T>
using Select_t = typename Select<Index, T>::type;

/**
 * @class WithSimpleReader
 * @brief A simple reader reactor mixin for reading a stream of data asynchronously.
 *
 * @details For a server reader, the channel will be closed with `grpc::Status::OK`
 * if a message cannot be read (i.e., `OnReadDone` is not ok).
 *
 * `T` can be a Server or Client Reactor. Note that for a client reader, you must
 * explicitly call `StartRead` once you create the reader object.
 *
 * @tparam T The base gRPC reactor class (e.g., `grpc::ServerReadReactor`,
 * `grpc::ClientBidiReactor`).
 */
template <typename T>
class WithSimpleReader : public T, public virtual WithReactorLock {
  public:
    using is_server = std::is_base_of<grpc::internal::ServerReactor, T>;

    // We select index 0, or index 1 in case of Bi-directional reactors.
    // I.e. T<X,Y,...> --> Y
    // T<X> --> X
    using R = Select_t<1, T>;

    WithSimpleReader() {
        if constexpr (is_server::value) {
            // Clients should not start immediately
            StartRead();
        }
    }

    void OnReadDone(bool ok) override {
        if (ok) {
            if (Read(&incoming_)) {
                absl::MutexLock lock(&this->reactor_lock_);
                T::StartRead(&incoming_);
            }
        } else {
            if constexpr (is_server::value) {
                // Call finish if we are a server without holding reactor_lock_
                T::Finish(grpc::Status::OK);
            }
        }
    }

    void StartRead() {
        absl::MutexLock lock(&this->reactor_lock_);
        T::StartRead(&incoming_);
    }

    /**
     * @brief Callback invoked when a new object is successfully read from the stream.
     *
     * @param read Pointer to the newly read object of type `R`.
     * @return `true` to continue reading the stream, `false` to stop.
     */
    virtual bool Read(const R* read) = 0;

  private:
    R incoming_;
};

/**
 * @class SimpleServerLambdaReader
 * @brief A simple server reader reactor utilizing lambda callbacks for read and completion events.
 *
 * @details This class automatically deletes itself upon completion (`OnDone`).
 * The channel is closed with `grpc::Status::OK` when the stream finishes normally.
 *
 * @tparam R The type of incoming request messages.
 * @tparam Base The underlying gRPC server read reactor base class.
 */
template <typename R, typename Base = grpc::ServerReadReactor<R>>
class SimpleServerLambdaReader : public WithSimpleReader<Base> {
    // A return other than OkStatus will Finish the stream with that status.
    using ReadCallback = absl::AnyInvocable<grpc::Status(const R*)>;
    using OnDoneCallback = absl::AnyInvocable<void() &&>;

  public:
    /**
     * @brief Constructs a `SimpleServerLambdaReader` with specified read and done callbacks.
     *
     * @param readFn Callback invoked for each incoming message. Returning a non-OK status finishes
     * the stream with that status.
     * @param doneFn Optional callback invoked when the stream completes before the reactor
     * self-deletes.
     */
    SimpleServerLambdaReader(ReadCallback readFn, OnDoneCallback doneFn = nullptr)
            : read_fn_(std::move(readFn)), done_fn_(std::move(doneFn)) {}

    virtual bool Read(const R* read) override {
        auto status = read_fn_(read);
        if (!status.ok()) {
            Base::Finish(status);
            return false;
        }
        return true;
    }

    virtual void OnDone() override {
        if (done_fn_) {
            std::move(done_fn_)();
        }
        delete this;
    }

  private:
    ReadCallback read_fn_;
    OnDoneCallback done_fn_;
};

/**
 * @class SimpleClientLambdaReader
 * @brief A simple client reader reactor utilizing lambda callbacks for asynchronous stream reading.
 *
 * @details This class automatically deletes itself upon completion (`OnDone`).
 *
 * @par Example Usage:
 * @code
 * grpc::ClientContext* context = client_->NewContext().release();
 * static google::protobuf::Empty empty;
 * auto read = new SimpleClientLambdaReader<PhoneEvent>(
 *         context,
 *         [](auto event) {
 *            std::cout << "Received event: " << event.ShortDebugString();
 *            return grpc::Status::OK;
 *         },
 *         [context](auto status) {
 *             std::cout << "Finished: " << status.error_message();
 *             delete context;
 *         });
 * service_->async()->receivePhoneEvents(context, &empty, read);
 * read->StartRead();
 * read->StartCall();
 * @endcode
 *
 * @tparam R The type of incoming response messages.
 * @tparam Base The underlying gRPC client read reactor base class.
 */
template <typename R, typename Base = grpc::ClientReadReactor<R>>
class SimpleClientLambdaReader : public WithSimpleReader<Base> {
    using ReadCallback = absl::AnyInvocable<grpc::Status(const R*)>;
    using OnDoneCallback = absl::AnyInvocable<void(::grpc::Status) &&>;

  public:
    /**
     * @brief Constructs a `SimpleClientLambdaReader` with a shared context and callbacks.
     *
     * @param context Shared pointer to the gRPC client context.
     * @param readFn Callback invoked for each incoming message.
     * @param doneFn Optional callback invoked upon stream completion with the final gRPC status.
     */
    SimpleClientLambdaReader(std::shared_ptr<grpc::ClientContext> context, ReadCallback readFn,
                             OnDoneCallback doneFn = nullptr)
            : context_(std::move(context))
            , read_fn_(std::move(readFn))
            , done_fn_(std::move(doneFn)) {}

    virtual bool Read(const R* read) override {
        auto status = read_fn_(read);
        if (!status.ok()) {
            Base::Finish(status);
            return false;
        }
        return true;
    }

    virtual void OnDone(const grpc::Status& status) override {
        if (done_fn_) {
            std::move(done_fn_)(status);
        }
        delete this;
    }

    virtual void TryCancel() { context_->TryCancel(); }

  private:
    std::shared_ptr<grpc::ClientContext> context_;
    ReadCallback read_fn_;
    OnDoneCallback done_fn_;
};

/**
 * @class WithSimpleQueueWriter
 * @brief A simple asynchronous writer mixin where outgoing objects are queued and written
 * sequentially.
 *
 * @details Important considerations when using this mixin:
 * - You will not be explicitly notified when an individual object has completed writing to the
 * wire.
 * - The internal queue will grow unbounded if the enqueue rate exceeds the underlying gRPC
 * network transmission rate and max_queue_size is 0.
 * - **Capacity & In-flight Semantics (`max_queue_size`)**: When `status_.writing` is in
 * progress, the front element (`write_queue_.front()`) is currently in-flight on the gRPC wire and
 * owned by the reactor; it cannot be modified or dropped until `OnWriteDone()`. Therefore,
 * `max_queue_size` bounds the number of queued pending messages. When the queue reaches
 * `max_queue_size`, new writes replace the latest pending message at the back of the queue.
 * Consequently, total `write_queue_.size()` (and `QueueSize()`) can reach up to `max_queue_size +
 * 1` (1 in-flight item + `max_queue_size` pending items; e.g., when `max_queue_size == 1`, size can
 * be 2: 1 in-flight and 1 pending).
 * - When `recycle_size > 0`, sent or evicted message objects are preserved in an internal recycle
 * pool and can be retrieved via `AcquireMessage()`. **Important**: Recycled objects can (and will)
 * contain stale data from previous transmissions. It is up to the caller (or populate function) to
 * properly initialize the recycled object (e.g. overwriting or clearing fields as needed). This
 * behavior is intentional and by design to allow reusing existing `std::string` buffers, repeated
 * collections, or allocated memory across writes without repetitive heap allocation.
 *
 * @tparam T The base gRPC reactor class (e.g., `grpc::ServerWriteReactor`,
 * `grpc::ClientBidiReactor`).
 * @tparam max_queue_size The maximum number of items to keep in the write queue (bounds pending
 * queue backlog; 0 = unbounded).
 * @tparam recycle_size The maximum number of recycled items to retain for reuse (0 = disabled).
 */
template <typename T, size_t max_queue_size = 0, size_t recycle_size = 0>
class WithSimpleQueueWriter : public T, public virtual WithReactorLock {
  public:
    // We always select index 0 of the T<X,...>
    using W = Select_t<0, T>;

    struct empty_recycle_storage {};
    struct recycle_storage {
        std::vector<W> queue;
    };

    void OnWriteDone(bool ok) override {
        {
            absl::MutexLock lock(&this->reactor_lock_);
            if constexpr (recycle_size > 0) {
                if (recycled_.queue.size() < recycle_size) {
                    recycled_.queue.push_back(std::move(write_queue_.front()));
                }
            }
            write_queue_.pop_front();
            status_.writing = false;
        }
        NextWrite();
    }

    /**
     * @brief Enqueues the specified object for asynchronous writing on the gRPC thread pool.
     *
     * @param msg The message object of type `W` to write.
     */
    void Write(const W& msg) {
        {
            absl::MutexLock lock(&this->reactor_lock_);
            if (status_.IsTerminated()) {
                return;
            }
        }
        if constexpr (recycle_size > 0) {
            W recycled = this->AcquireMessage();
            recycled = msg;  // Overwrites and reuses buffers
            Write(std::move(recycled));
        } else {
            Write(W(msg));
        }
    }

    /**
     * @brief Enqueues the specified object for asynchronous writing on the gRPC thread pool,
     * forwarding the message.
     *
     * @param msg The message object of type `W` to write.
     */
    void Write(W&& msg) {
        {
            absl::MutexLock lock(&this->reactor_lock_);
            if (status_.IsTerminated()) {
                return;
            }
            // Drop previously queued pending element if the pending backlog is full.
            // Note: If status_.writing is true, write_queue_.front() is in-flight on the wire and
            // owned by the gRPC reactor, so pending_count tracks unwritten elements waiting in the
            // queue. For real-time streams (e.g. graphics/video frames), replacing the unwritten
            // frame at the back ensures the newest frame is delivered with lowest latency while
            // preserving monotonic arrival order without queue reordering.
            if (max_queue_size > 0) {
                const size_t in_flight = status_.writing ? 1 : 0;
                const size_t pending_count = write_queue_.size() - in_flight;
                if (pending_count >= max_queue_size) {
                    if constexpr (recycle_size > 0) {
                        if (recycled_.queue.size() < recycle_size) {
                            recycled_.queue.push_back(std::move(write_queue_.back()));
                        }
                    }
                    write_queue_.pop_back();
                }
            }
            write_queue_.push_back(std::move(msg));
        }
        NextWrite();
    }

    /**
     * @brief Acquires a recycled message instance if available, or constructs a default one.
     *
     * @details **Important**: Recycled objects can (and will) contain stale data from previous
     * transmissions. It is up to the caller or populate function to properly initialize the
     * recycled object (e.g. overwriting fields or clearing unneeded data). This is by design to
     * allow reusing existing `std::string` buffers, allocated arrays, or internal capacity
     * without incurring repetitive heap allocations.
     *
     * @return A message of type `W` (which may contain stale data with its previously allocated
     * buffer capacity intact if recycled, or a default-constructed `W` otherwise).
     */
    W AcquireMessage() {
        if constexpr (recycle_size > 0) {
            absl::MutexLock lock(&this->reactor_lock_);
            if (!recycled_.queue.empty()) {
                W msg = std::move(recycled_.queue.back());
                recycled_.queue.pop_back();
                return msg;
            }
        }
        return W();
    }

    /**
     * @brief Returns an unused message instance back to the recycle pool if capacity permits.
     *
     * @param msg The unused message object of type `W` to recycle.
     */
    void RecycleMessage(W&& msg) {
        if constexpr (recycle_size > 0) {
            const absl::MutexLock reactor_lock(this->reactor_lock_);
            if (!status_.IsTerminated() && recycled_.queue.size() < recycle_size) {
                recycled_.queue.push_back(std::move(msg));
            }
        }
    }

    /**
     * @brief Returns the number of items currently pending in the write queue.
     */
    size_t QueueSize() const {
        absl::MutexLock lock(&this->reactor_lock_);
        return write_queue_.size();
    }

    /**
     * @brief Returns the number of items currently in the recycle queue.
     */
    size_t RecycleQueueSize() const {
        if constexpr (recycle_size > 0) {
            absl::MutexLock lock(&this->reactor_lock_);
            return recycled_.queue.size();
        }
        return 0;
    }

    /**
     * @brief Returns true if the stream has finished.
     */
    bool IsFinished() const {
        absl::MutexLock lock(&this->reactor_lock_);
        return status_.finished;
    }

    /**
     * @brief Returns true if the client cancelled the stream.
     */
    bool IsCancelled() const {
        absl::MutexLock lock(&this->reactor_lock_);
        return status_.cancelled;
    }

    /**
     * @brief Finishes the stream. Thread-safe and idempotent.
     */
    template <typename... Args>
    void Finish(Args&&... args) {
        if (SetFinished()) {
            T::Finish(std::forward<Args>(args)...);
        }
    }

  protected:
    struct StatusFlags {
        bool writing : 1 {false};
        bool finished : 1 {false};
        bool cancelled : 1 {false};

        bool IsTerminated() const { return finished || cancelled; }
    };

    /**
     * @brief Marks the writer as finished.
     *
     * @return true if this call transitioned the writer to finished; false if already finished.
     */
    bool SetFinished() {
        absl::MutexLock lock(&this->reactor_lock_);
        if (status_.finished) {
            return false;
        }
        status_.finished = true;
        return true;
    }

    /**
     * @brief Marks the writer as cancelled.
     *
     * @return true if this call transitioned the writer to cancelled; false if already cancelled.
     */
    bool SetCancelled() {
        absl::MutexLock lock(&this->reactor_lock_);
        if (status_.cancelled) {
            return false;
        }
        status_.cancelled = true;
        return true;
    }

  private:
    void NextWrite() {
        absl::MutexLock lock(&this->reactor_lock_);
        if (!status_.IsTerminated() && !status_.writing && !write_queue_.empty()) {
            status_.writing = true;
            T::StartWrite(&write_queue_.front());
        }
    }

    std::deque<W> write_queue_ ABSL_GUARDED_BY(this->reactor_lock_);
    StatusFlags status_ ABSL_GUARDED_BY(this->reactor_lock_){};
    using storage_type =
            std::conditional_t<(recycle_size > 0), recycle_storage, empty_recycle_storage>;

    // Takes 0 bytes when recycle_size == 0 due to [[no_unique_address]]
    // https://en.cppreference.com/cpp/language/attributes/no_unique_address
    [[no_unique_address]] storage_type recycled_ ABSL_GUARDED_BY(this->reactor_lock_);
};

/**
 * @class SimpleClientWriter
 * @brief A simple client writer reactor managing an asynchronous outgoing stream.
 *
 * @tparam W The type of outgoing request messages.
 */
template <typename W>
class SimpleClientWriter : public WithSimpleQueueWriter<grpc::ClientWriteReactor<W>> {
  public:
    /**
     * @brief Constructs a `SimpleClientWriter` with a shared client context.
     *
     * @param context Shared pointer to the gRPC client context.
     */
    SimpleClientWriter(std::shared_ptr<::grpc::ClientContext> context)
            : context_(std::move(context)) {}

    /**
     * @brief Retrieves the raw pointer to the underlying gRPC client context.
     *
     * @return Pointer to `grpc::ClientContext`.
     */
    ::grpc::ClientContext* context() { return context_.get(); }

  private:
    std::shared_ptr<::grpc::ClientContext> context_;
};

/**
 * @brief A bidirectional server stream composed of both simple reader and queue writer mixins.
 *
 * @tparam R The type of incoming request messages.
 * @tparam W The type of outgoing response messages.
 * @tparam max_queue_size The maximum number of items to keep in the write queue (0 = unbounded).
 * @tparam recycle_size The maximum number of items to keep in the recycle pool (0 = disabled).
 */
template <typename R, typename W, size_t max_queue_size = 0, size_t recycle_size = 0>
using SimpleServerBidiStream =
        WithSimpleQueueWriter<WithSimpleReader<grpc::ServerBidiReactor<R, W>>, max_queue_size,
                              recycle_size>;

/**
 * @brief A simple server reader reactor alias.
 *
 * @tparam R The type of incoming request messages.
 */
template <typename R>
using SimpleServerReader = WithSimpleReader<grpc::ServerReadReactor<R>>;

/**
 * @brief A simple server writer reactor alias.
 *
 * @tparam W The type of outgoing response messages.
 * @tparam max_queue_size The maximum number of items to keep in the write queue (0 = unbounded).
 * @tparam recycle_size The maximum number of items to keep in the recycle pool (0 = disabled).
 */
template <typename W, size_t max_queue_size = 0, size_t recycle_size = 0>
using SimpleServerWriter =
        WithSimpleQueueWriter<grpc::ServerWriteReactor<W>, max_queue_size, recycle_size>;

/**
 * @class SimpleServerLambdaWriter
 * @brief Server-side streaming write reactor utilizing lambda callbacks for stream initiation,
 * cancellation, and completion.
 *
 * Automatically deletes itself upon completion (`OnDone()`). Enables inline streaming
 * without declaring a bespoke subclass of `WithSimpleQueueWriter`:
 *
 * @code
 * return new SimpleServerLambdaWriter<LogEntry>(
 *     [this](auto* writer) {
 *         // Start: register callback or initiate producer -> writer->Write(msg);
 *     },
 *     [this]() {
 *         // Optional: client cancelled stream
 *     });
 * @endcode
 *
 * The `Base` template parameter defaults to `grpc::ServerWriteReactor<W>`, but can be
 * substituted with an in-memory mock reactor (e.g. `FakeWriteReactor<W>`) for hermetic unit
 * testing.
 *
 * @tparam W The type of outgoing response messages.
 * @tparam Base The underlying reactor base class (defaults to `grpc::ServerWriteReactor<W>`).
 * @tparam max_queue_size Maximum pending messages in write queue (0 = unbounded).
 * @tparam recycle_size Maximum recycled messages to retain (0 = disabled).
 */
template <typename W, typename Base = grpc::ServerWriteReactor<W>, size_t max_queue_size = 0,
          size_t recycle_size = 0>
class SimpleServerLambdaWriter : public WithSimpleQueueWriter<Base, max_queue_size, recycle_size> {
  public:
    using StartCallback = absl::AnyInvocable<void(SimpleServerLambdaWriter*)>;
    // These are 'use only once' callbacks see http://go/totw/191 for details.
    using OnCancelCallback = absl::AnyInvocable<void() &&>;
    using OnDoneCallback = absl::AnyInvocable<void() &&>;

    /**
     * @brief Constructs a `SimpleServerLambdaWriter` with specified start, cancel, and done
     * callbacks.
     *
     * @param startFn Callback invoked once construction is complete, passing `this` to begin
     * streaming.
     * @param cancelFn Optional callback invoked when the client cancels the stream.
     * @param doneFn Optional callback invoked when the stream completes before the reactor
     * self-deletes.
     */
    SimpleServerLambdaWriter(StartCallback startFn, OnCancelCallback cancelFn = nullptr,
                             OnDoneCallback doneFn = nullptr)
            : cancel_fn_(std::move(cancelFn)), done_fn_(std::move(doneFn)) {
        if (startFn) {
            startFn(this);
        }
    }

    /**
     * @brief Callback invoked by gRPC when the client cancels the stream. Thread-safe and
     * idempotent.
     */
    void OnCancel() override {
        if (!this->SetCancelled()) {
            return;
        }
        if (cancel_fn_) {
            std::move(cancel_fn_)();
        }
        this->Finish(::grpc::Status::CANCELLED);
    }

    /**
     * @brief Final completion callback invoked by gRPC. Executes `done_fn_` and self-deletes.
     */
    void OnDone() override {
        if (done_fn_) {
            std::move(done_fn_)();
        }
        delete this;
    }

  private:
    OnCancelCallback cancel_fn_;
    OnDoneCallback done_fn_;
};

/**
 * @brief A bidirectional client stream composed of both simple reader and queue writer mixins.
 *
 * @tparam R The type of incoming response messages.
 * @tparam W The type of outgoing request messages.
 * @tparam max_queue_size The maximum number of items to keep in the write queue (0 = unbounded).
 * @tparam recycle_size The maximum number of items to keep in the recycle pool (0 = disabled).
 */
template <typename R, typename W, size_t max_queue_size = 0, size_t recycle_size = 0>
using SimpleClientBidiStream =
        WithSimpleQueueWriter<WithSimpleReader<grpc::ClientBidiReactor<W, R>>, max_queue_size,
                              recycle_size>;

/**
 * @class ErrorServerWriter
 * @brief A simple server writer reactor that immediately finishes the stream with a status.
 *
 * @tparam W The type of outgoing response messages.
 */
template <typename W>
class ErrorServerWriter final : public ::grpc::ServerWriteReactor<W> {
  public:
    explicit ErrorServerWriter(::grpc::Status status) { this->Finish(status); }
    void OnDone() override { delete this; }
};
