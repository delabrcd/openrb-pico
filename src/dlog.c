#include "dlog.h"

#include <stdarg.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"

#define DLOG_UART uart1
#define DLOG_TX_PIN 24
#define DLOG_RX_PIN 25

#define DLOG_SIZE 8192u  // must be a power of two
#define DLOG_MASK (DLOG_SIZE - 1u)

// SPSC ring: core1 (producer) only advances head; core0 (consumer) only advances
// tail. 32-bit aligned loads/stores are atomic on Cortex-M0+, so no lock needed.
static volatile char dlog_buf[DLOG_SIZE];
static volatile uint32_t dlog_head;  // next write index (producer)
static volatile uint32_t dlog_tail;  // next read index (consumer)

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

    uint32_t len = (n < (int)sizeof(tmp)) ? (uint32_t)n : (uint32_t)sizeof(tmp) - 1u;
    uint32_t h = dlog_head;
    uint32_t tail = dlog_tail;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t nh = (h + 1u) & DLOG_MASK;
        if (nh == tail) break;  // buffer full -> drop the rest
        dlog_buf[h] = tmp[i];
        h = nh;
    }
    dlog_head = h;  // single store publishes the batch
    return n;
}

void dlog_drain(void) {
    uint32_t t = dlog_tail;
    uint32_t h = dlog_head;
    while (t != h) {
        uart_putc_raw(DLOG_UART, dlog_buf[t]);
        t = (t + 1u) & DLOG_MASK;
    }
    dlog_tail = t;
}
