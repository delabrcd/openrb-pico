#ifndef ORB_DLOG_H_
#define ORB_DLOG_H_

// Deferred logger for debugging the timing-critical PIO-USB host on core1.
// Producers (esp. core1) format into a lock-free SPSC RAM ring buffer with no
// UART/mutex/blocking; core0 drains it to the debug UART from the main loop.
// This avoids starving the core1 USB SOF interrupt the way blocking printf does.

void dlog_init(void);                       // call once on core0 before launching core1
int dlog_printf(const char *fmt, ...);      // producer (any core) — used as CFG_TUSB_DEBUG_PRINTF
void dlog_drain(void);                       // consumer — call from the core0 main loop

#endif  // ORB_DLOG_H_
