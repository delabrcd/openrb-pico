#ifndef XBOX_CONTROLLER_DRIVER_H
#define XBOX_CONTROLLER_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

// clang-format off
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "tusb_option.h"
#include "common/tusb_types.h"
// clang-format on

#include "xbox_one_protocol.h"

bool xboxh_receive_report(uint8_t daddr, uint8_t idx);
bool xboxh_send_report(uint8_t daddr, uint8_t idx, const void *report, uint16_t len);

TU_ATTR_WEAK void xboxh_mount_cb(uint8_t dev_addr, uint8_t instance);
TU_ATTR_WEAK void xboxh_umount_cb(uint8_t dev_addr, uint8_t instance);

TU_ATTR_WEAK void xboxh_packet_received_cb(uint8_t idx, const xbox_packet_t *data,
                                           const uint8_t ndata);

TU_ATTR_WEAK void xboxh_packet_sent_cb(uint8_t idx, const xbox_packet_t *data, const uint8_t ndata);

void xboxh_init(void);
bool xboxh_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf,
                uint16_t max_len);
bool xboxh_set_config(uint8_t daddr, uint8_t itf_num);
bool xboxh_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
void xboxh_close(uint8_t daddr);

#endif  // XBOX_CONTROLLER_DRIVER_H