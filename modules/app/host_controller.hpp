#pragma once

/*
 * orb::app::HostController — the core1 USB-HOST controller loop (P4 slice 3 of the
 * rearchitect/di-classes DI pass). Owns the state that used to live as main.cpp file
 * statics/globals for the Xbox-controller host path: the host-side scratch packet
 * (host_out_packet), the runtime-recovery liveness timestamps (g_host_last_rx_us /
 * g_host_rx_count), and the host_recovery_task/usb_host_task function-static recovery
 * state. Dispatched from the TinyUSB xboxh_* callbacks via a SeamAnchor<HostController>
 * (see host_controller.cpp) exactly like the drums MIDI seam (drums_midi_seam.cpp).
 *
 * core1 hot path: on_mount/on_umount/on_packet_received/on_packet_sent/run() all run on
 * core1 (the ONLY task pinned there, no FreeRTOS context switches). No blocking calls, no
 * heap, no vtable dispatch through this class -- the SeamAnchor thunk in
 * host_controller.cpp calls these methods directly on the concrete type. run()/
 * configure_host()/host_recovery() must stay exactly as timing-sensitive as the main.cpp
 * usb_host_task/configure_host/host_recovery_task they replace (see host_controller.cpp
 * for the ORB_BOARD_ID-gated recovery cut).
 *
 * Board-agnostic header: no #if ORB_BOARD_ID, no orb_bsp.h. The board-conditional method
 * bodies live in host_controller.cpp, which is compiled per-board (see
 * modules/app/CMakeLists.txt's ORB_APP_BOARD_SOURCES).
 */

#include <atomic>
#include <cstdint>

#include "actuators.hpp"        // orb::board::Actuators
#include "adapter_ctx.h"        // orb::service::AdapterState
#include "osal/queue.hpp"       // orb::osal::Queue
#include "packet_queue.h"       // orb::driver::DeviceTxFifo
#include "recovery.hpp"         // orb::app::RecoveryState
#include "xbox_one_protocol.h"  // XboxPacket

namespace orb::app {

class HostController {
   public:
    HostController(orb::service::AdapterState& adapter,
                    orb::driver::DeviceTxFifo<XboxPacket, 16>& tx_fifo,
                    orb::osal::Queue<XboxPacket, 8>& host_tx, RecoveryState& recovery_state,
                    orb::board::Actuators& actuators)
        : adapter_(adapter),
          tx_fifo_(tx_fifo),
          host_tx_(host_tx),
          recovery_state_(recovery_state),
          actuators_(actuators) {}

    HostController(const HostController&) = delete;
    HostController& operator=(const HostController&) = delete;

    // extern "C" xboxh_*_cb seam dispatch (host_controller.cpp) -- core1 only.
    void on_mount(std::uint8_t dev_addr, std::uint8_t instance);
    void on_umount(std::uint8_t dev_addr, std::uint8_t instance);
    void on_packet_received(std::uint8_t idx, const XboxPacket& data, std::uint8_t ndata);
    void on_packet_sent(std::uint8_t idx, const XboxPacket& data, std::uint8_t ndata);

    // The core1 USB host task body (was usb_host_task). Never returns.
    void run();

   private:
    void configure_host();
    void handle_controller_packet_running(const XboxPacket& data);
    void host_recovery();

    orb::service::AdapterState& adapter_;
    orb::driver::DeviceTxFifo<XboxPacket, 16>& tx_fifo_;
    orb::osal::Queue<XboxPacket, 8>& host_tx_;
    RecoveryState& recovery_state_;
    orb::board::Actuators& actuators_;

    // Host-side scratch packet for handle_controller_packet_running -- core1 ONLY (was
    // main.cpp's file-static host_out_packet; kept separate from System's device-side
    // out_packet so a core0 device-RX handler can't tear a controller-input packet being
    // built concurrently on core1).
    XboxPacket host_out_packet_;

    // Runtime (non-reboot) host recovery liveness timestamps -- core1 only, relaxed
    // atomics (was g_host_last_rx_us / g_host_rx_count in main.cpp).
    std::atomic<std::uint32_t> host_last_rx_us_{0};
    std::atomic<std::uint32_t> host_rx_count_{0};

    // host_recovery_task's former function-static locals -- now core1-only members.
    bool recovering_ = false;
    bool gave_up_ = false;
    std::uint8_t attempts_ = 0;
    std::uint32_t last_attempt_us_ = 0;
    std::uint32_t last_rx_count_ = 0;

    // usb_host_task's former local -- now a core1-only member.
    std::uint32_t last_reinit_us_ = 0;
};

// Bind the single HostController instance (owned by orb::app::System) to the seam anchor
// used by the extern "C" xboxh_*_cb thunks in host_controller.cpp. Call once, at init,
// before tuh_init() runs (usb_host_task -> HostController::run -> configure_host()).
void bind_host_controller(HostController& hc);

}  // namespace orb::app
