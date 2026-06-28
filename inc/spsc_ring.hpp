/*
 * SpscRing<T, N> — a single-producer / single-consumer lock-free ring, header-only,
 * BSS-resident, no heap. It centralises the exact discipline currently hand-rolled in
 * three places (src/dlog.c's two per-core rings and src/usb_log.c's core0->core1 ring;
 * see docs/features/cpp-overhaul.md D3).
 *
 * The contract — reproduced verbatim from the proven C rings, do not weaken it:
 *   - N is a power of two; one slot is reserved so "full" is unambiguous (head+1==tail).
 *   - Exactly ONE core produces (advances head_) and exactly ONE core consumes
 *     (advances tail_). They touch different 32-bit words.
 *   - On Cortex-M0+ a naturally-aligned 32-bit load/store is atomic, and the two cores
 *     are effectively strongly ordered, so no lock/critical-section is needed even
 *     though both cores touch the ring concurrently. This is the same reasoning the
 *     existing rings rely on (src/dlog.c:18-23).
 *   - A full ring DROPS rather than blocks — this is what keeps a chatty core1 from
 *     ever stalling the PIO-USB SOF. write() returns how many elements it accepted.
 *   - The producer publishes a whole batch with a SINGLE head store; the consumer
 *     commits with a SINGLE tail store. Never partially publish.
 *
 * Cursors are kept as `volatile uint32_t`, identical to the validated C code. Migrating
 * to std::atomic<uint32_t> (relaxed/acquire/release) is the more-correct option and is
 * free on M0+ for plain load/store, BUT must be verified to emit the same ldr/str with
 * no library call or stray dmb before adoption — that's open-question Q3 in the spec and
 * is deliberately NOT done here, so this header is a faithful, low-risk drop-in.
 *
 * core1 safety: nothing here locks, allocates, or calls the SDK. Safe to use from the
 * PIO-USB core. T must be trivially copyable (small POD: bytes, notes, handles).
 *
 * Consumer usage patterns (both needed by the existing callers):
 *
 *   // (a) drain everything now (dlog-style): emit each contiguous run, then commit.
 *   for (auto run = ring.readable(); run.len; run = ring.readable()) {
 *       for (uint32_t i = 0; i < run.len; i++) sink(run.ptr[i]);
 *       ring.consume(run.len);
 *   }
 *
 *   // (b) conditional commit (usb_log-style): peek a run, do fallible I/O, commit only
 *   //     the bytes that actually landed.
 *   auto run = ring.readable();
 *   uint32_t n = min(run.len, chunk_cap);
 *   if (f_write(run.ptr, n) == OK) ring.consume(n);
 */
#ifndef OPENRB_SPSC_RING_HPP
#define OPENRB_SPSC_RING_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace orb {

template <typename T, size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "SpscRing: N must be a power of two");
    static_assert(N >= 2, "SpscRing: N must be >= 2");
    static_assert(std::is_trivially_copyable<T>::value,
                  "SpscRing: T must be trivially copyable (no-heap embedded policy)");

   public:
    static constexpr uint32_t kMask = static_cast<uint32_t>(N) - 1u;

    // Usable capacity is N-1 (one slot reserved to disambiguate full from empty).
    static constexpr uint32_t capacity() { return static_cast<uint32_t>(N) - 1u; }

    //--- producer side (ONE core only) --------------------------------------------
    // Push one element. Returns false (and drops it) if the ring is full.
    bool push(const T& v) {
        uint32_t h = head_;
        uint32_t nh = (h + 1u) & kMask;
        if (nh == tail_) return false;  // full
        buf_[h] = v;
        head_ = nh;  // single store publishes
        return true;
    }

    // Best-effort bulk write: copy as many of `n` as fit, drop the rest, publish the
    // whole accepted batch with a single head store. Returns the count accepted.
    uint32_t write(const T* src, uint32_t n) {
        uint32_t h = head_;
        uint32_t tail = tail_;  // snapshot the consumer cursor once
        uint32_t i = 0;
        for (; i < n; i++) {
            uint32_t nh = (h + 1u) & kMask;
            if (nh == tail) break;  // ring full -> drop the rest
            buf_[h] = src[i];
            h = nh;
        }
        head_ = h;  // single store publishes the batch
        return i;
    }

    //--- consumer side (ONE core only) --------------------------------------------
    bool empty() const { return head_ == tail_; }

    // A view of the largest CONTIGUOUS readable run starting at the tail (stops at the
    // physical buffer wrap; call again to get the post-wrap run). len==0 means empty.
    struct Run {
        const volatile T* ptr;
        uint32_t len;
    };
    Run readable() const {
        uint32_t t = tail_;
        uint32_t h = head_;  // snapshot the producer cursor once
        if (t == h) return Run{nullptr, 0};
        uint32_t end = (h > t) ? h : static_cast<uint32_t>(N);  // contiguous up to head or wrap
        return Run{&buf_[t], end - t};
    }

    // Pop one element into out. Returns false if empty.
    bool pop(T& out) {
        uint32_t t = tail_;
        if (t == head_) return false;  // empty
        out = const_cast<const T&>(buf_[t]);
        tail_ = (t + 1u) & kMask;  // single store commits
        return true;
    }

    // Advance the tail by n (commit consumption). Single store. n must not exceed the
    // length most recently reported by readable().
    void consume(uint32_t n) { tail_ = (tail_ + n) & kMask; }

   private:
    volatile T buf_[N];
    volatile uint32_t head_ = 0;  // next write index (producer)
    volatile uint32_t tail_ = 0;  // next read index (consumer)
};

}  // namespace orb

#endif  // OPENRB_SPSC_RING_HPP
