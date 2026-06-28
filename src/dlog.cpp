#include "dlog.h"

#include <cstdarg>
#include <cstdio>

#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/uart.h"
#include "spsc_ring.hpp"

#define DLOG_UART uart1
#define DLOG_TX_PIN 24
#define DLOG_RX_PIN 25

#define DLOG_NRING 2      // one SPSC ring per core
#define DLOG_SIZE 16384u  // per ring; must be a power of two

// One lock-free SPSC ring per core (SpscRing<char,N>, inc/spsc_ring.hpp). Each core only
// ever produces into its own ring (advancing that ring's head); core0 is the sole
// consumer and drains both rings (advancing each tail). Producer and consumer touch
// different 32-bit words -- aligned 32-bit loads/stores are atomic on Cortex-M0+ -- so no
// lock is needed even though both cores log concurrently. A full ring drops the rest of
// the message rather than blocking, keeping the core1 USB SOF running. (The ring contract
// and drop-on-full behaviour are unchanged from the hand-rolled rings this replaced.)
static orb::SpscRing<char, DLOG_SIZE> s_ring[DLOG_NRING];

static dlog_sink_t dlog_sink = nullptr;

void dlog_set_sink(dlog_sink_t sink) { dlog_sink = sink; }

void dlog_init(void) {
    uart_init(DLOG_UART, 115200);
    gpio_set_function(DLOG_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(DLOG_RX_PIN, GPIO_FUNC_UART);
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
    s_ring[ring].write(tmp, len);  // best-effort batch write; single head store publishes
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
                uart_putc_raw(DLOG_UART, c);
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
