#pragma once

#include "common/tusb_common.h"
#include "orb_c_api.h"
#include "packet_queue.h"  // orb::driver::DeviceTxFifo
#include "xbox_one_protocol.h"

// Transitional: xbox_device_driver is now C++ (orb::driver), but xboxd_send_task is still
// called from main.cpp and the weak callbacks below are implemented in main.cpp. The seam
// gives all of them C linkage so the symbols resolve across the C++ TUs until this driver
// is folded into the app-layer orchestration. (No-op in C.)
ORB_C_BEGIN

bool xboxd_send_task();

TU_ATTR_WEAK bool xboxd_packet_received_cb(uint8_t rhport, const XboxPacket *buf,
                                           uint32_t xferred_bytes);

TU_ATTR_WEAK void xboxd_on_reset_cb();

ORB_C_END

namespace orb::driver {

// Binds the System-owned device TX fifo to this TU's SeamAnchor. Called once from
// orb::app::bind_usb_seams(), before the device stack (tud_init) starts draining it -- see
// modules/core/seam_anchor.hpp.
void bind_device_tx_fifo(orb::driver::DeviceTxFifo<XboxPacket, 16>& fifo);

}  // namespace orb::driver
