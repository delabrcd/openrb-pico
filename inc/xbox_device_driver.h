#ifndef XBOX_DEVICE_DRIVER_H
#define XBOX_DEVICE_DRIVER_H

#include "common/tusb_common.h"
#include "xbox_one_protocol.h"

bool xboxd_send_task();

TU_ATTR_WEAK bool xboxd_packet_received_cb(uint8_t rhport, const xbox_packet_t *buf,
                                           uint32_t xferred_bytes);

#endif