#pragma once

#include <cstdint>

// clang-format off
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "tusb_option.h"
#include "common/tusb_types.h"
// clang-format on

#include "orb_c_api.h"
#include "xbox_one_protocol.h"

// Transitional C/C++ seam (see orb_c_api.h): the TinyUSB host class callbacks
// (xboxh_open/set_config/xfer_cb/close/init) are the vendor seam, and main.cpp calls
// xboxh_send_report / xboxh_reinit_controller / xboxh_in_error_streak /
// xboxh_clear_error_streak / the weak xboxh_*_cb hooks across the boundary. Wrapping the
// declarations in extern "C" keeps every one of these symbols C-linkage from both the
// .cpp definition and its callers.
ORB_C_BEGIN

bool xboxh_receive_report(uint8_t daddr, uint8_t idx);
bool xboxh_send_report(uint8_t daddr, uint8_t idx, const void *report, uint16_t len);

TU_ATTR_WEAK void xboxh_mount_cb(uint8_t dev_addr, uint8_t instance);
TU_ATTR_WEAK void xboxh_umount_cb(uint8_t dev_addr, uint8_t instance);

TU_ATTR_WEAK void xboxh_packet_received_cb(uint8_t idx, const xbox_packet_t *data,
                                           const uint8_t ndata);

TU_ATTR_WEAK void xboxh_packet_sent_cb(uint8_t idx, const xbox_packet_t *data, const uint8_t ndata);

bool xboxh_init(void);
bool xboxh_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf,
                uint16_t max_len);
bool xboxh_set_config(uint8_t daddr, uint8_t itf_num);
bool xboxh_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
void xboxh_close(uint8_t daddr);

// Consecutive failed interrupt-IN transfers (a wedged hub fails the poll continuously;
// a healthy idle controller produces none). Fast "wedged" signal for host_recovery_task.
uint32_t xboxh_in_error_streak(void);
void xboxh_clear_error_streak(void);

bool xboxh_reinit_controller(uint8_t daddr, uint8_t idx);

void xboxh_power_on_controllers();
void xboxh_power_off_controllers();

void xboxh_reset_controllers();

ORB_C_END
