#pragma once

#include <stdint.h>

// Deferred logger for debugging the timing-critical PIO-USB host on core1.
// Each core formats into its OWN RAM ring buffer (SpscRing<char,N>) with no
// UART/mutex/blocking; core0 drains both rings to the debug UART from housekeeping_task.
// The cross-core producer/consumer relationship needs no lock (aligned 32-bit cursor
// loads/stores are atomic on M0+). core1's ring has a single producer (usb_host_task);
// core0's ring is multi-producer, so dlog_printf serializes core0 writes with a brief
// core0-only interrupt mask (see src/dlog.cpp) -- never disabling interrupts on core1.
// This avoids starving the core1 USB SOF interrupt the way blocking printf does.

// Plain C++ free functions (every consumer is a C++ TU); defined in dlog.cpp.
void dlog_init(void);                       // call once on core0 before launching core1
int dlog_printf(const char *fmt, ...);      // producer (either core)
void dlog_drain(void);                       // consumer — call from a core0 task only (housekeeping_task)

// Optional secondary sink: drained bytes are delivered here in chunks as well as
// to the UART. Used to mirror the log to a USB flash drive (usb_log). core0/drain
// context.
typedef void (*dlog_sink_t)(const uint8_t *data, uint32_t len);
void dlog_set_sink(dlog_sink_t sink);

