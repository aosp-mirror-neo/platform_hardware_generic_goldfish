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

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <ranges>
#include <thread>
#include <vector>

#include "goldfish/async/qemu_event_loop.h"
extern "C" {

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "migration/snapshot.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"


// Windows workarounds
#ifdef shutdown
#undef shutdown
#endif
#ifdef close
#undef close
#endif
// IWYU pragma: end_keep
// clang-format on
}

// This file provides a fake implementation of the QEMU main loop functions
// that are used by QemuEventLoop. This allows for unit testing of the
// event loop logic without running inside the full QEMU environment.
//
// The implementation is based on a manually advanced clock (`sFakeClockMs`)
// and lists of pending timers and bottom-halves (BHs). The test code is
// responsible for calling `fake_qemu_advance_ms()` to process events.
//
// This fake is not thread-safe for concurrent calls to `fake_qemu_advance_ms`,
// but it is safe for other threads to schedule timers/BHs while the clock is
// being advanced.

// A fake QEMU Bottom-Half (BH) structure. A BH is a callback that is
// scheduled to run on the next iteration of the event loop.
struct QEMUBH {
    QEMUBHFunc* cb;
    void* opaque;
    bool deleted;    // True if the BH has been deleted and should be ignored.
    bool scheduled;  // True if the BH is currently scheduled to run.
};

struct QemuSnapshotProgressEntry {
    QemuSnapshotProgressHandler* cb;
    void* opaque;
};

namespace {

// A recursive mutex to protect all access to the global fake QEMU state.
// This allows callbacks to schedule new timers/BHs safely.
std::recursive_mutex sMutex;

// The master fake clock, in milliseconds. Tests manually advance this clock.
std::atomic<int64_t> sFakeClockMs = 0;

// A list of all active QEMU timers.
std::list<QEMUTimer*> sTimers;
// A list of all created BHs, used for tracking and cleanup.
std::list<QEMUBH*> sBottomHalves;
// A vector of BHs that are scheduled to run in the current event loop tick.
std::vector<QEMUBH*> sScheduledBHs;
// Registered snapshot progress handlers.
std::list<QemuSnapshotProgressEntry*> sSnapshotProgressHandlers;

// --- I/O Loop State ---
struct FdHandler {
    IOHandler* read_cb;
    IOHandler* write_cb;
    void* opaque;
};

std::map<int, FdHandler> sFdHandlers;

}  // namespace

extern "C" {

// --- Public Test Control Functions ---

void fake_qemu_advance_ms(int64_t ms) {
    int64_t deadline = sFakeClockMs + ms;
    while (sFakeClockMs < deadline) {
        // Step 1: Process a single tick of the fake clock
        sFakeClockMs++;

        // Step 2: Check and dispatch any expired timers
        std::vector<QEMUBH*> scheduled_bh_from_timers;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            for (auto* timer : sTimers) {
                if (timer->scale && timer->expire_time <= sFakeClockMs) {
                    timer->scale = 0;
                    scheduled_bh_from_timers.push_back(qemu_bh_new(timer->cb, timer->opaque));
                }
            }
        }

        // Step 3: Process I/O events without blocking
        std::vector<struct pollfd> pollfds;
        std::map<int, FdHandler> current_handlers;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            current_handlers = sFdHandlers;
        }

        for (const auto& [fd, handler] : current_handlers) {
            struct pollfd pfd = {0};
            pfd.fd = fd;
            if (handler.read_cb) {
                pfd.events |= POLLIN;
            }
            if (handler.write_cb) {
                pfd.events |= POLLOUT;
            }
            pollfds.push_back(pfd);
        }

        // Use a non-blocking poll call with a 0ms timeout
#ifdef _WIN32
        int ret = WSAPoll(pollfds.data(), pollfds.size(), 0);
#else
        int ret = poll(pollfds.data(), pollfds.size(), 0);
#endif

        if (ret > 0) {
            for (const auto& p : pollfds) {
                if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
                    FdHandler handler;
                    {
                        std::lock_guard<std::recursive_mutex> lock(sMutex);
                        handler = sFdHandlers[p.fd];
                    }
                    if (handler.read_cb) {
                        VLOG(1) << "Scheduling Read callback";
                        qemu_bh_schedule(qemu_bh_new(handler.read_cb, handler.opaque));
                    }
                }
                if (p.revents & (POLLOUT | POLLHUP | POLLERR)) {
                    FdHandler handler;
                    {
                        std::lock_guard<std::recursive_mutex> lock(sMutex);
                        handler = sFdHandlers[p.fd];
                    }
                    if (handler.write_cb) {
                        VLOG(1) << "Scheduling Write callback";
                        qemu_bh_schedule(qemu_bh_new(handler.write_cb, handler.opaque));
                    }
                }
            }
        }

        // Step 4: Process all scheduled BHs (from both timers and I/O)
        for (auto* bh : scheduled_bh_from_timers) {
            if (!bh->deleted) {
                bh->scheduled = false;
                bh->cb(bh->opaque);
            }
        }

        std::vector<QEMUBH*> scheduled_bhs;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            scheduled_bhs = sScheduledBHs;
            sScheduledBHs.clear();
        }
        for (auto* bh : std::views::reverse(scheduled_bhs)) {
            if (!bh->deleted) {
                bh->scheduled = false;
                bh->cb(bh->opaque);
            }
        }
    }
}

void fake_qemu_fire_snapshot_progress(const QemuSnapshotProgress* progress) {
    std::list<QemuSnapshotProgressEntry*> handlers;
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);
        handlers = sSnapshotProgressHandlers;
    }
    for (auto* entry : handlers) {
        if (entry && entry->cb) {
            entry->cb(entry->opaque, progress);
        }
    }
}

void fake_qemu_reset() {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    fake_qemu_advance_ms(1);
    sFakeClockMs = 0;

    if (!sTimers.empty()) {
        LOG(FATAL) << "You are leaking timers!";
    }
    sTimers.clear();

    for (auto* bh : sBottomHalves) {
        delete bh;
    }
    sBottomHalves.clear();
    sScheduledBHs.clear();
    sFdHandlers.clear();
    for (auto* entry : sSnapshotProgressHandlers) {
        delete entry;
    }
    sSnapshotProgressHandlers.clear();
}

// --- Fake QEMU API Implementations ---

void qemu_set_fd_handler(int fd, IOHandler* fd_read, IOHandler* fd_write, void* opaque) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    if (!fd_read && !fd_write) {
        sFdHandlers.erase(fd);
    } else {
        sFdHandlers[fd] = {fd_read, fd_write, opaque};
    }
}

int64_t qemu_clock_get_ns(QEMUClockType type) {
    return sFakeClockMs ? sFakeClockMs * 1000000 : 0;
}

void timer_init_full(QEMUTimer* ts, QEMUTimerListGroup* timer_list_group, QEMUClockType type,
                     int scale, int attributes, QEMUTimerCB* cb, void* opaque) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    ts->cb = cb;
    ts->opaque = opaque;
    ts->scale = 0;
    ts->attributes = 0;
    sTimers.push_back(ts);
}

void timer_mod(QEMUTimer* ts, int64_t expire_time) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    if (!ts) return;
    ts->expire_time = expire_time;
    ts->scale = 1;
}

void timer_del(QEMUTimer* ts) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    if (!ts) return;
    sTimers.remove(ts);
}

QEMUBH* qemu_bh_new_full(QEMUBHFunc* cb, void* opaque, const char* name,
                         MemReentrancyGuard* reentrancy_guard) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    auto* bh = new QEMUBH{cb, opaque, false, false};
    sBottomHalves.push_back(bh);
    return bh;
}

void qemu_bh_schedule(QEMUBH* bh) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    if (!bh || bh->scheduled || bh->deleted) {
        return;
    }
    bh->scheduled = true;
    VLOG(1) << "Scheduling bh";
    sScheduledBHs.push_back(bh);
}

void qemu_bh_cancel(QEMUBH* bh) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    if (!bh) return;
    bh->scheduled = false;
    auto it = std::remove(sScheduledBHs.begin(), sScheduledBHs.end(), bh);
    sScheduledBHs.erase(it, sScheduledBHs.end());
}

void qemu_bh_delete(QEMUBH* bh) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    if (!bh) return;
    bh->deleted = true;
    qemu_bh_cancel(bh);
    sBottomHalves.remove(bh);
    delete bh;
}

bool bql_locked() {
    return false;
}

void bql_lock_impl(const char* file, int line) {}

void bql_unlock() {}

QemuSnapshotProgressEntry* add_snapshot_progress_handler(QemuSnapshotProgressHandler* cb,
                                                         void* opaque) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    auto* entry = new QemuSnapshotProgressEntry{cb, opaque};
    sSnapshotProgressHandlers.push_back(entry);
    return entry;
}

void del_snapshot_progress_handler(QemuSnapshotProgressEntry* entry) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    if (!entry) {
        return;
    }
    sSnapshotProgressHandlers.remove(entry);
    delete entry;
}

}  // extern "C"
