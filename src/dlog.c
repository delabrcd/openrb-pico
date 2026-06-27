#include "dlog.h"

#include <stdarg.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/uart.h"

#define DLOG_UART uart1
#define DLOG_TX_PIN 24
#define DLOG_RX_PIN 25

#define DLOG_NRING 2     // one SPSC ring per core
#define DLOG_SIZE 16384u  // per ring; must be a power of two
#define DLOG_MASK (DLOG_SIZE - 1u)

// One lock-free SPSC ring per core. Each core only ever produces into its own
// ring (advancing that ring's head); core0 is the sole consumer and drains both
// rings (advancing each tail). Producer and consumer touch different 32-bit
// variables -- aligned 32-bit loads/stores are atomic on Cortex-M0+ -- so no
// lock is needed even though both cores log concurrently. A full ring drops the
// rest of the message rather than blocking, keeping the core1 USB SOF running.
static volatile char dlog_buf[DLOG_NRING][DLOG_SIZE];
static volatile uint32_t dlog_head[DLOG_NRING];  // next write index (producer)
static volatile uint32_t dlog_tail[DLOG_NRING];  // next read index (consumer)

static dlog_sink_t dlog_sink = NULL;

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
    uint32_t h = dlog_head[ring];
    uint32_t tail = dlog_tail[ring];
    for (uint32_t i = 0; i < len; i++) {
        uint32_t nh = (h + 1u) & DLOG_MASK;
        if (nh == tail) break;  // ring full -> drop the rest
        dlog_buf[ring][h] = tmp[i];
        h = nh;
    }
    dlog_head[ring] = h;  // single store publishes the batch
    return n;
}

void dlog_drain(void) {
    uint8_t chunk[128];
    uint32_t cn = 0;
    for (uint32_t ring = 0; ring < DLOG_NRING; ring++) {
        uint32_t t = dlog_tail[ring];
        uint32_t h = dlog_head[ring];
        while (t != h) {
            char c = dlog_buf[ring][t];
            uart_putc_raw(DLOG_UART, c);
            if (dlog_sink) {
                chunk[cn++] = (uint8_t)c;
                if (cn == sizeof(chunk)) {
                    dlog_sink(chunk, cn);
                    cn = 0;
                }
            }
            t = (t + 1u) & DLOG_MASK;
        }
        dlog_tail[ring] = t;
    }
    if (dlog_sink && cn) dlog_sink(chunk, cn);
}
