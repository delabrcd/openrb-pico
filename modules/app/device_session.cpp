/*
 * orb::app::DeviceSession — board-agnostic bodies for the core0 USB-DEVICE session loop
 * (P4 slice 4 of the rearchitect/di-classes DI pass). Moved VERBATIM out of
 * modules/app/main.cpp: out_packet / xboxd_on_reset_cb / handle_auth / handle_identify /
 * handle_init / handle_running / handle_xboxd_packet / xboxd_packet_received_cb /
 * announce_task / usb_device_task. Only the substitutions needed to hang off
 * DeviceSession member state were applied -- no new indirection, same control flow.
 *
 * The single orb::core::SeamAnchor<DeviceSession> is bound once, from
 * orb::app::bind_usb_seams() (system.cpp), before tud_init() runs on core0 -- exactly the
 * core1 HostController seam's pattern (host_controller.cpp).
 */
#include "device_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>  // std::to_underlying

#include <bsp/board_api.h>   // board_millis
#include <device/usbd.h>     // tud_task_ext / TUD_OPT_RHPORT
#include "hardware/watchdog.h"  // watchdog_reboot

#include "adapter.h"            // adapter_state_t, announce_interval
#include "identifiers.h"        // identifiers_get / identifiers_get_n / identifiers_get_announce
#include "orb_log.h"
#include "xbox_device_driver.h"  // xboxd_send_task + the weak xboxd_*_cb decls

#include "core/seam_anchor.hpp"

namespace orb::app {
namespace {
orb::core::SeamAnchor<DeviceSession> g_device;
}  // namespace

void bind_device_session(DeviceSession& ds) { g_device.bind(ds); }

DeviceSession::DeviceSession(orb::service::AdapterState& adapter,
                             orb::driver::DeviceTxFifo<XboxPacket, 16>& tx_fifo,
                             orb::osal::Queue<XboxPacket, 8>& host_tx,
                             orb::board::Actuators& actuators,
                             orb::service::InstrumentManager& instruments)
    : adapter_(adapter),
      tx_fifo_(tx_fifo),
      host_tx_(host_tx),
      actuators_(actuators),
      instruments_(instruments) {
    std::ranges::fill(out_packet_.wire(), std::uint8_t{0});
}

// ---- DeviceSession method bodies (moved verbatim from main.cpp) -----------------------

void DeviceSession::on_reset() {
    actuators_.set_auth_led(false);

    // TODO CDD - look into a better way of reinitializing the USB Host stack than a hard reset
    if (adapter_.state() != adapter_state_t::STATE_INIT &&
        adapter_.state() != adapter_state_t::STATE_NONE)
        watchdog_reboot(0, 0, 10);
}

void DeviceSession::handle_auth(const XboxPacket* packet) {
    if (packet->frame().command == frame_command_e::CMD_AUTHENTICATE &&
        packet->frame().length == 2 &&
        packet->data()[3] == 2 && packet->data()[4] == 1 && packet->data()[5] == 0) {
        actuators_.set_auth_led(true);

        LOG_INFO(CAT_DEV, "AUTHENTICATED!");
        adapter_.set_state(adapter_state_t::STATE_RUNNING);

        instruments_.notify_all(out_packet_);
    }

    LOG_DBG(CAT_DEV, "Sending controller %d bytes", packet->length);
    host_tx_.send(*packet);  // drained + sent on core1 by HostController::run
    return;
}

void DeviceSession::handle_identify(const XboxPacket* packet) {
    switch (packet->frame().command) {
        case frame_command_e::CMD_IDENTIFY:
        case frame_command_e::CMD_ACKNOWLEDGE:
            if (identify_sequence_ >= identifiers_get_n()) {
                LOG_INFO(CAT_DEV, "Starting identify sequence over");
                identify_sequence_ = 0;
            }
            identifiers_get(identify_sequence_, &out_packet_);
            tx_fifo_.write(out_packet_);
            identify_sequence_++;
            break;
        case frame_command_e::CMD_AUTHENTICATE:
            LOG_INFO(CAT_DEV, "Moving to Authenticate");
            adapter_.set_state(adapter_state_t::STATE_AUTHENTICATING);
            return handle_auth(packet);
            break;
        default:
            break;
    }
    return;
}

void DeviceSession::handle_init(const XboxPacket* packet) {
    switch (packet->frame().command) {
        case frame_command_e::CMD_IDENTIFY:
            LOG_INFO(CAT_DEV, "Moving to Identify");
            adapter_.set_state(adapter_state_t::STATE_IDENTIFYING);
            return handle_identify(packet);
        default:
            break;
    }
}

void DeviceSession::handle_running(const XboxPacket* packet) {
    switch (packet->frame().command) {
        case frame_command_e::CMD_POWER_MODE:
            if (packet->power().data == std::to_underlying(power_mode_e::POWER_OFF)) {
                adapter_.set_state(adapter_state_t::STATE_POWER_OFF);
                actuators_.set_auth_led(false);
                actuators_.set_usb_host(false);
            }
            break;
        case frame_command_e::CMD_ACKNOWLEDGE:
            host_tx_.send(*packet);  // drained + sent on core1 by HostController::run
            break;
        case frame_command_e::CMD_LIST_CONNECTED_INSTRUMENTS:
            instruments_.notify_all(out_packet_);
            break;
        case frame_command_e::CMD_LIST_INSTRUMENT:
            instruments_.notify_single(static_cast<instruments_e>(packet->data()[4]), out_packet_);
            break;
        default:
            break;
    }
    return;
}

void DeviceSession::handle_xboxd_packet(const XboxPacket* packet) {
    switch (adapter_.state()) {
        case adapter_state_t::STATE_NONE:
            return;
        case adapter_state_t::STATE_INIT:
            return handle_init(packet);
        case adapter_state_t::STATE_IDENTIFYING:
            return handle_identify(packet);
        case adapter_state_t::STATE_AUTHENTICATING:
            return handle_auth(packet);
        case adapter_state_t::STATE_RUNNING:
            return handle_running(packet);
        default:
            break;
    }
    return;
}

bool DeviceSession::on_packet_received(const XboxPacket& data, std::uint32_t xferred_bytes) {
    if (xferred_bytes < sizeof(frame_t)) return false;

    handle_xboxd_packet(&data);
    return true;
}

void DeviceSession::announce() {
    if (adapter_.state() != adapter_state_t::STATE_INIT) return;

    if (std::chrono::milliseconds(board_millis() - last_announce_time_) > orb::service::announce_interval) {
        if (adapter_.controller_idx() < UINT8_MAX) {
            LOG_INFO(CAT_DEV, "ANNOUNCING");
            identifiers_get_announce(&out_packet_);
            tx_fifo_.write(out_packet_);
            last_announce_time_ = board_millis();
        }
    }
}

// USB device task. Runs on core0 alongside the send drain -- both touch the device
// endpoint (tud_task processes events; xboxd_send_task claims the IN endpoint), and
// TinyUSB device-stack access must be serialized. tud_task_ext(4) blocks on the device
// event queue but wakes at least every 4ms to drain device_tx to the console.
void DeviceSession::run() {
    while (true) {
        tud_task_ext(4, false);
        xboxd_send_task();
    }
}

}  // namespace orb::app

// --- extern "C" TinyUSB device controller seam ---------------------------------------------
// TinyUSB calls these by C symbol; each reaches the single DeviceSession instance (owned
// by orb::app::System) through the SeamAnchor bound at init. Direct concrete call, no
// vtable -- core0 hot path.

extern "C" bool xboxd_packet_received_cb(uint8_t rhport, const XboxPacket* buf,
                                          uint32_t xferred_bytes) {
    (void)rhport;
    return orb::app::g_device ? orb::app::g_device->on_packet_received(*buf, xferred_bytes) : false;
}

extern "C" void xboxd_on_reset_cb() {
    if (orb::app::g_device) orb::app::g_device->on_reset();
}
