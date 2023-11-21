#ifndef XBOX_CONTROLLER_DRIVER_H
#define XBOX_CONTROLLER_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "tusb_option.h"
#include "xbox_one_protocol.h"

bool xboxh_send_report(uint8_t daddr, uint8_t idx, const void *report, uint16_t len);

TU_ATTR_WEAK void xboxh_mount_cb(uint8_t dev_addr, uint8_t instance);
TU_ATTR_WEAK void xboxh_umount_cb(uint8_t dev_addr, uint8_t instance);

TU_ATTR_WEAK void xboxh_packet_received_cb(uint8_t idx, const xbox_packet_t *data,
                                           const uint8_t ndata);

TU_ATTR_WEAK void xboxh_packet_sent_cb(uint8_t idx, const xbox_packet_t *data, const uint8_t ndata);

#endif  // XBOX_CONTROLLER_DRIVER_H