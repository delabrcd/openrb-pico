#ifndef ORB_DLOG_H_
#define ORB_DLOG_H_

#include <stdint.h>

// Deferred logger for debugging the timing-critical PIO-USB host on core1.
// Each core formats into its OWN lock-free SPSC RAM ring buffer with no
// UART/mutex/blocking; core0 drains both rings to the debug UART from
// housekeeping_task. Per-core rings keep each ring single-producer/single-consumer, so both
// cores can log concurrently without a lock. This avoids starving the core1 USB
// SOF interrupt the way blocking printf does.

void dlog_init(void);                       // call once on core0 before launching core1
int dlog_printf(const char *fmt, ...);      // producer (either core) — also CFG_TUSB_DEBUG_PRINTF
void dlog_drain(void);                       // consumer — call from a core0 task only (housekeeping_task)

// Optional secondary sink: drained bytes are delivered here in chunks as well as
// to the UART. Used to mirror the log to a USB flash drive (usb_log). core0/drain
// context.
typedef void (*dlog_sink_t)(const uint8_t *data, uint32_t len);
void dlog_set_sink(dlog_sink_t sink);

#endif  // ORB_DLOG_H_
