#ifndef ORB_USB_LOG_H_
#define ORB_USB_LOG_H_

#include <stdbool.h>
#include <stdint.h>

#include "orb_c_api.h"  // ORB_C_BEGIN/END (impl is C++ now; callers are C)

// Stream the deferred log to a USB flash drive plugged into the hub, via FatFs
// over the TinyUSB MSC host. Replaces the onboard-QSPI approach -- USB writes
// never disable XIP, so the whole flash/XIP cross-core hazard is gone.
//
// Data path: core0 drains dlog and pushes bytes into a lock-free SPSC RAM ring
// (usb_log_write, the dlog sink). core1 (which owns the USB host stack) drains the
// ring and writes it to LOG.TXT (usb_log_task, called from the core1 loop). All
// FatFs / tuh_msc work happens on core1.

ORB_C_BEGIN

// dlog sink: producer side, core0. Pushes bytes into the ring (drops on overflow).
void usb_log_write(const uint8_t *data, uint32_t len);

// Consumer side, core1: mount the stick on first sight, then drain the ring to
// LOG.TXT. Call once per core1 loop iteration, after tuh_task().
void usb_log_task(void);

// Gate flushing on/off (e.g. disable during the auth handshake -- the ring keeps
// buffering while disabled, and drains once re-enabled). Default enabled.
void usb_log_set_enabled(bool enabled);

ORB_C_END

#endif  // ORB_USB_LOG_H_
