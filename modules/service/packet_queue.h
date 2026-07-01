#pragma once

#include <atomic>  // std::atomic_signal_fence (compiler-only ordering, no emitted code)
#include <cstdint>
#include <type_traits>

#include "osal/mutex.hpp"  // orb::osal::Mutex / ScopedLock
#include "xbox_one_protocol.h"  // XboxPacket

namespace orb::driver {

// Device-bound TX fifo for the console-facing endpoint. It replaces the retired
// CREATE_GENERIC_FIFO(xbox, XboxPacket, 16, /*rd_mtx=*/false, /*wr_mtx=*/true)
// instantiation (a tu_fifo + OSAL write-mutex) with the exact same semantics:
//
//   - MULTI-WRITER enqueue: both cores feed this fifo (core0 device-RX handlers +
//     core1 host->device translation), so write() is serialized by a FreeRTOS mutex
//     (orb::osal::Mutex via a ScopedLock) -- the one lock the old fifo also kept.
//   - SINGLE drainer: core0 xboxd_send_task is the only consumer, so read/peek/advance
//     are lock-free (a redundant read mutex deadlocked under tinyusb 0.18's blocking
//     osal_pico mutex -- that asymmetry is preserved here by design).
//   - Non-blocking, NO overwrite: a full ring drops the write (returns 0), like tu_fifo.
//
// Memory discipline mirrors orb::core::SpscRing (inc/core/spsc_ring.hpp): naturally
// aligned 32-bit cursors are `volatile` (atomic single-word load/store on Cortex-M0+),
// and the two cores are effectively strongly ordered, so no critical section is needed
// on the read side. SpscRing makes its slot storage `volatile` too, but that only works
// for scalar slots -- a `volatile` struct array can't be assigned (no volatile-qualified
// implicit copy assignment), so the payload buffer here is plain and the slot copy is
// kept ordered against the cursor publish/consume with std::atomic_signal_fence (a
// compiler-only barrier that emits no code; CPU ordering is already given by the M0+
// strong-ordering assumption above). The canonical SpscRing is strictly single-producer,
// so it can't be reused verbatim for a multi-writer fifo; the mutex provides the
// producer-vs-producer ordering it assumes.
//
// Cursors are monotonic and masked by the capacity (a power of two), so all `Capacity`
// slots are usable -- preserving the old depth of 16 exactly (count == Capacity is full,
// count == 0 is empty; no reserved slot).
template <typename T, uint32_t Capacity>
class DeviceTxFifo {
    static_assert((Capacity & (Capacity - 1u)) == 0u, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable<T>::value,
                  "DeviceTxFifo: T must be trivially copyable (no-heap embedded policy)");

    static constexpr uint32_t kMask = Capacity - 1u;

   public:
    // Create the write mutex. Call once, before/while the scheduler runs (matches the
    // old xbox_fifo_init -> osal_mutex_create timing).
    void init() {
        mutex_.create();
        head_ = 0;
        tail_ = 0;
    }

    // Producer (BOTH cores) -- serialized by the mutex. Returns 1 if enqueued, 0 if full.
    uint32_t write(const T &item) {
        orb::osal::ScopedLock guard(mutex_);
        const uint32_t head = head_;
        if (head - tail_ >= Capacity) return 0;  // full -> drop, no overwrite
        buffer_[head & kMask] = item;
        std::atomic_signal_fence(std::memory_order_release);  // slot store precedes publish
        head_ = head + 1u;                                    // single store publishes the slot
        return 1;
    }

    // Consumer (core0 only) -- lock-free. Returns 1 if an item was read, 0 if empty.
    uint32_t read(T &out) {
        const uint32_t tail = tail_;
        if (head_ == tail) return 0;  // empty
        std::atomic_signal_fence(std::memory_order_acquire);  // slot read after head_ check
        out = buffer_[tail & kMask];
        std::atomic_signal_fence(std::memory_order_release);  // slot read precedes slot release
        tail_ = tail + 1u;                                    // single store releases the slot
        return 1;
    }

    // Consumer (core0 only) -- read without consuming. Returns 1 if an item is available.
    uint32_t peek(T &out) const {
        const uint32_t tail = tail_;
        if (head_ == tail) return 0;  // empty
        std::atomic_signal_fence(std::memory_order_acquire);  // slot read after head_ check
        out = buffer_[tail & kMask];
        return 1;
    }

    // Consumer (core0 only) -- drop the head-of-line item (paired with peek).
    void advance() {
        const uint32_t tail = tail_;
        if (head_ == tail) return;  // empty -> nothing to advance
        std::atomic_signal_fence(std::memory_order_release);  // prior peek read precedes release
        tail_ = tail + 1u;
    }

    uint32_t count() const { return head_ - tail_; }
    bool empty() const { return head_ == tail_; }
    bool full() const { return (head_ - tail_) >= Capacity; }

    // Consumer-side drain of everything currently queued.
    void clear() { tail_ = head_; }

   private:
    T buffer_[Capacity];          // payload slots (ordering via the signal fences above)
    volatile uint32_t head_ = 0;  // producer index (monotonic, masked by kMask)
    volatile uint32_t tail_ = 0;  // consumer index
    orb::osal::Mutex mutex_;
};

// Depth preserved verbatim from the retired #define XBOX_FIFO_SIZE 16.
constexpr uint32_t kXboxFifoDepth = 16;

}  // namespace orb::driver

// --- public API (plain C++ free functions) ------------------------------------------------
// Reached from the device driver (xbox_device_driver) and the feature TUs (drums, guitar,
// instrument_manager, main); thin forwarders into the single DeviceTxFifo instance owned by
// orb::app::System (definitions live in the app-layer bridge TU, modules/app/system.cpp --
// service/ TUs never reach into orb::app directly).
void xbox_fifo_init(void);
uint32_t xbox_fifo_read(XboxPacket *buffer);   // 1 if read, 0 if empty (single drainer)
uint32_t xbox_fifo_peek(XboxPacket *buffer);   // 1 if available, 0 if empty
void xbox_fifo_advance(void);                     // drop the head-of-line item
uint32_t xbox_fifo_write(const XboxPacket *buffer);  // 1 if enqueued, 0 if full (multi-writer)
uint32_t xbox_fifo_count(void);
bool xbox_fifo_empty(void);
bool xbox_fifo_full(void);
void xbox_fifo_clear(void);
