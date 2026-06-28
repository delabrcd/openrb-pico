#ifndef ADAPTER_CTX_H
#define ADAPTER_CTX_H

// Cross-core adapter state shared between core0 (USB device stack / device-RX
// callbacks) and core1 (USB host stack / controller mount-umount-RX). Previously a
// loose set of `volatile` globals in main.c; consolidated here behind accessors.
//
// Concurrency (RP2040, dual-core Cortex-M0+ where aligned 32-bit/byte loads/stores
// are atomic):
//  - adapter_state: written core0, read both cores. Plain volatile enum word; lock-free.
//  - controller idx+addr: written core1 (mount/umount), read core0 + core1. Packed into
//    ONE 32-bit word so a replug can't be observed half-updated (torn read); published
//    with a single store and sentinel for "no controller".
//  - alive / seen: written core1, read core0. Single volatile bool each; lock-free.
//  - reinit_pending: producer and consumer are both core1; volatile bool; lock-free.

#include <stdbool.h>
#include <stdint.h>

#include "adapter.h"  // adapter_state_t

void adapter_ctx_init(void);  // set state=STATE_NONE, no controller, flags false

adapter_state_t adapter_get_state(void);
void adapter_set_state(adapter_state_t s);

// controller slot (idx+addr packed). returns false if no controller currently tracked.
bool adapter_get_controller(uint8_t *idx, uint8_t *addr);
void adapter_set_controller(uint8_t idx, uint8_t addr);
void adapter_clear_controller(uint8_t idx);  // only clears if idx matches the tracked one
uint8_t adapter_get_controller_idx(void);    // convenience: tracked idx or UINT8_MAX

bool adapter_controller_alive(void);
void adapter_set_controller_alive(bool v);
bool adapter_controller_seen(void);
void adapter_set_controller_seen(bool v);

void adapter_request_reinit(void);  // producer: set reinit_pending
bool adapter_take_reinit(void);     // consumer: read-and-clear, returns prior value

#endif  // ADAPTER_CTX_H
