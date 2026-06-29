#pragma once

#include "common/tusb_common.h"
#include "orb_c_api.h"
#include "xbox_one_protocol.h"

// Transitional: xbox_device_driver is now C++ (orb::driver), but xboxd_send_task is still
// called from main.cpp and the weak callbacks below are implemented in main.cpp. The seam
// gives all of them C linkage so the symbols resolve across the C++ TUs until this driver
// is folded into the app-layer orchestration. (No-op in C.)
ORB_C_BEGIN

bool xboxd_send_task();

TU_ATTR_WEAK bool xboxd_packet_received_cb(uint8_t rhport, const xbox_packet_t *buf,
                                           uint32_t xferred_bytes);

TU_ATTR_WEAK void xboxd_on_reset_cb();

ORB_C_END
