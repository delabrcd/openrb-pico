#pragma once

/*
 * orb::app::DeviceSession — the core0 USB-DEVICE session (P4 slice 4 of the
 * rearchitect/di-classes DI pass). Owns the state that used to live as main.cpp file
 * statics/globals for the Xbox-device-facing path: the device-side scratch packet
 * (out_packet), the identify-sequence cursor (handle_identify's function-static
 * identify_sequence), and the announce heartbeat timestamp (announce_task's
 * function-static last_announce_time). Dispatched from the TinyUSB xboxd_* callbacks via
 * a SeamAnchor<DeviceSession> (see device_session.cpp) exactly like the core1
 * HostController seam (host_controller.cpp).
 *
 * core0 hot path: on_packet_received/on_reset/announce/run() all run on core0 (the USB
 * device stack's task). No heap, no vtable dispatch through this class -- the SeamAnchor
 * thunk in device_session.cpp calls these methods directly on the concrete type.
 *
 * Board-agnostic header: no #if ORB_BOARD_ID, no orb_bsp.h. The .cpp is board-agnostic
 * too and compiled as a normal INTERFACE source (see modules/app/CMakeLists.txt).
 */

#include <cstdint>

#include "adapter_ctx.h"        // orb::service::AdapterState
#include "actuators.hpp"        // orb::board::Actuators
#include "osal/queue.hpp"       // orb::osal::Queue
#include "packet_queue.h"       // orb::driver::DeviceTxFifo
#include "instrument_manager.h"  // orb::service::InstrumentManager
#include "xbox_one_protocol.h"  // XboxPacket

namespace orb::app {

class DeviceSession {
   public:
    DeviceSession(orb::service::AdapterState& adapter,
                  orb::driver::DeviceTxFifo<XboxPacket, 16>& tx_fifo,
                  orb::osal::Queue<XboxPacket, 8>& host_tx, orb::board::Actuators& actuators,
                  orb::service::InstrumentManager& instruments);

    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;

    // extern "C" xboxd_*_cb seam dispatch (device_session.cpp) -- core0 only.
    bool on_packet_received(const XboxPacket& data, std::uint32_t xferred_bytes);
    void on_reset();

    // The core0 announce heartbeat (was announce_task) -- called from housekeeping_task.
    void announce();

    // The core0 USB device task body (was usb_device_task). Never returns.
    void run();

   private:
    void handle_init(const XboxPacket* p);
    void handle_identify(const XboxPacket* p);
    void handle_auth(const XboxPacket* p);
    void handle_running(const XboxPacket* p);
    void handle_xboxd_packet(const XboxPacket* p);

    orb::service::AdapterState& adapter_;
    orb::driver::DeviceTxFifo<XboxPacket, 16>& tx_fifo_;
    orb::osal::Queue<XboxPacket, 8>& host_tx_;
    orb::board::Actuators& actuators_;
    orb::service::InstrumentManager& instruments_;

    // Device-side scratch packet: built by the core0 device-RX handlers (auth / identify /
    // running) before being copied into the cross-core TX fifo -- touched ONLY by the USB
    // device task (was main.cpp's file-static out_packet).
    XboxPacket out_packet_;

    // Separate scratch for the announce heartbeat. announce() is driven from the housekeeping
    // task, a DIFFERENT core0 task than run()/the RX handlers, so it gets its own buffer rather
    // than sharing out_packet_ -- no cross-task interleaving on a single packet, even though the
    // task priorities happen to preclude a mid-build preemption today.
    XboxPacket announce_packet_;

    // handle_identify's former function-static local -- now a core0-only member.
    std::uint8_t identify_sequence_ = 0;

    // announce_task's former function-static local -- now a core0-only member.
    unsigned long last_announce_time_ = 0;
};

// Bind the single DeviceSession instance (owned by orb::app::System) to the seam anchor
// used by the extern "C" xboxd_*_cb thunks in device_session.cpp. Call once, at init,
// before tud_init() runs.
void bind_device_session(DeviceSession& ds);

}  // namespace orb::app
