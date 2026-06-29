#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "orb_c_api.h"          // ORB_C_BEGIN/END — C-linkage seam (impl is C++ now)
#include "xbox_one_protocol.h"  // xbox_packet_t

// Device-bound TX fifo for the console-facing endpoint. The implementation
// (src/packet_queue.cpp) is a multi-writer-safe, single-drainer ring of xbox_packet_t.
// These free functions keep their original signatures + C linkage so the still-C driver
// (xbox_device_driver) and the C++ feature TUs (drums/guitar/instrument_manager/main)
// keep calling them unchanged. The seam retires once the last C caller becomes C++.
ORB_C_BEGIN

void xbox_fifo_init(void);
uint32_t xbox_fifo_read(xbox_packet_t *buffer);   // 1 if read, 0 if empty (single drainer)
uint32_t xbox_fifo_peek(xbox_packet_t *buffer);   // 1 if available, 0 if empty
void xbox_fifo_advance(void);                     // drop the head-of-line item
uint32_t xbox_fifo_write(const xbox_packet_t *buffer);  // 1 if enqueued, 0 if full (multi-writer)
uint32_t xbox_fifo_count(void);
bool xbox_fifo_empty(void);
bool xbox_fifo_full(void);
void xbox_fifo_clear(void);

ORB_C_END

