#include "dlog.h"

#include <cstdarg>
#include <cstdio>
#include <optional>

// hardware/structs/sio.h is kept here (not routed through the HAL) because sio_hw->cpuid
// is the only way to identify which core is writing. There is no hal:: concept for CPU
// core ID yet; adding one is left as a follow-on (needs hal::core::id() or similar).
#include "hardware/structs/sio.h"
#include "hal/platform.hpp"
#include "core/spsc_ring.hpp"

#define DLOG_UART uart1
#define DLOG_TX_PIN 24
#define DLOG_RX_PIN 25

#define DLOG_NRING 2      // one SPSC ring per core
#define DLOG_SIZE 16384u  // per ring; must be a power of two

// Debug UART handle — emplaced by dlog_init(). std::optional so uart_init +
// gpio_set_function run only after the system clock is stable (docs/architecture.md lifetime).
static std::optional<orb::hal::Uart> s_dlog_uart;

// One ring per core (SpscRing<char,N>, inc/spsc_ring.hpp). Each core produces only into
// its own ring (advancing that ring's head); core0 is the sole consumer and drains both
// rings (advancing each tail). Producer and consumer touch different 32-bit words --
// aligned 32-bit loads/stores are atomic on Cortex-M0+ -- so the cross-core producer/
// consumer relationship needs no lock. A full ring drops the rest of the message rather
// than blocking, keeping the core1 USB SOF running.
//
// Producer counts differ by core, which matters for SpscRing's single-producer rule:
//   - ring[1] (core1) has exactly ONE producer -- core1 runs only usb_host_task -- so it
//     is a genuine SPSC ring, no serialization needed.
//   - ring[0] (core0) is MULTI-producer: several preemptible core0 tasks (usb_device,
//     drum_input, housekeeping) plus the FreeRTOS hooks and init/recovery paths all log.
//     A context switch mid-write() would interleave two producers and regress head_, so
//     dlog_printf serializes the core0 write by masking THIS core's interrupts (below).
static orb::core::SpscRing<char, DLOG_SIZE> s_ring[DLOG_NRING];

static dlog_sink_t dlog_sink = nullptr;

void dlog_set_sink(dlog_sink_t sink) { dlog_sink = sink; }

void dlog_init(void) {
    s_dlog_uart.emplace(DLOG_UART, unsigned{DLOG_TX_PIN}, unsigned{DLOG_RX_PIN}, 115200u);
}

int dlog_printf(const char *fmt, ...) {
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return n;

    const uint32_t ring = sio_hw->cpuid & 1u;  // 0 on core0, 1 on core1
    uint32_t len = (n < (int)sizeof(tmp)) ? (uint32_t)n : (uint32_t)sizeof(tmp) - 1u;
    if (ring == 0u) {
        // core0 is multi-producer: mask THIS core's interrupts so a higher-priority core0
        // task can't preempt and interleave mid-write. The masked region is one ring write
        // (a bounded copy of up to ~160 bytes -- a few microseconds of core0 interrupt
        // latency under chatty logging); it touches only core0's PRIMASK (no cross-core
        // spinlock), so it never stalls core1 or its PIO-USB timing.
        orb::hal::IrqGuard guard;
        s_ring[0].write(tmp, len);
    } else {
        // core1 runs exactly one task -> genuine single producer, no masking (and we must
        // never disable interrupts on the PIO-USB core).
        s_ring[1].write(tmp, len);
    }
    return n;
}

void dlog_drain(void) {
    uint8_t chunk[128];
    uint32_t cn = 0;
    for (uint32_t ring = 0; ring < DLOG_NRING; ring++) {
        // Drain each contiguous run to the UART and (if set) the secondary sink, then
        // commit. readable() returns at most two runs per pass (pre/post wrap); looping
        // until empty matches the old "drain to head" behaviour. core1's logging is
        // sparse, and a full ring is bounded, so this terminates promptly.
        for (auto run = s_ring[ring].readable(); run.len; run = s_ring[ring].readable()) {
            for (uint32_t i = 0; i < run.len; i++) {
                char c = run.ptr[i];
                s_dlog_uart->write_byte(std::byte{static_cast<unsigned char>(c)});
                if (dlog_sink) {
                    chunk[cn++] = (uint8_t)c;
                    if (cn == sizeof(chunk)) {
                        dlog_sink(chunk, cn);
                        cn = 0;
                    }
                }
            }
            s_ring[ring].consume(run.len);  // single tail store commits the run
        }
    }
    if (dlog_sink && cn) dlog_sink(chunk, cn);
}
