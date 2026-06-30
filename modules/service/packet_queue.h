#pragma once

#include <cstdint>

#include "xbox_one_protocol.h"  // xbox_packet_t

// Device-bound TX fifo for the console-facing endpoint. The implementation
// (packet_queue.cpp) is a multi-writer-safe, single-drainer ring of xbox_packet_t.
// Plain C++ free functions (every consumer is a C++ TU): the device driver
// (xbox_device_driver) and the feature TUs (drums/guitar/instrument_manager/main).
void xbox_fifo_init(void);
uint32_t xbox_fifo_read(xbox_packet_t *buffer);   // 1 if read, 0 if empty (single drainer)
uint32_t xbox_fifo_peek(xbox_packet_t *buffer);   // 1 if available, 0 if empty
void xbox_fifo_advance(void);                     // drop the head-of-line item
uint32_t xbox_fifo_write(const xbox_packet_t *buffer);  // 1 if enqueued, 0 if full (multi-writer)
uint32_t xbox_fifo_count(void);
bool xbox_fifo_empty(void);
bool xbox_fifo_full(void);
void xbox_fifo_clear(void);

