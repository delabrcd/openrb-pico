#ifndef PACKET_QUEUE_H_
#define PACKET_QUEUE_H_

#include <stdbool.h>

#include "tusb_option.h"
#include "xbox_one_protocol.h"

void xbox_fifo_init();
uint32_t xbox_fifo_read(xbox_packet_t *buffer);
uint32_t xbox_fifo_write(const xbox_packet_t *buffer);
uint32_t xbox_fifo_count();
bool xbox_fifo_empty();
bool xbox_fifo_full();
void xbox_fifo_clear();
uint32_t xbox_fifo_peek(xbox_packet_t *buffer);
void xbox_fifo_advance();
#endif  // PACKET_QUEUE_H
