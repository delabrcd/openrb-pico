#pragma once

/*
 * orb::app::Housekeeping — the core0 low-priority periodic background task (P4 slice 5 of
 * the rearchitect/di-classes DI pass). Groups the controller-announce heartbeat (gated to
 * STATE_INIT, fires ~every 2s internally, see DeviceSession::announce()), the bounded
 * warm-reset zombie recovery (RebootRecovery::service()), and draining the deferred log to
 * UART/USB (dlog_drain()) -- all coarse periodic chores that used to live together as
 * main.cpp's housekeeping_task.
 *
 * Board-agnostic: no #if ORB_BOARD_ID, no orb_bsp.h. Compiled as a normal INTERFACE source
 * (see modules/app/CMakeLists.txt).
 */

#include "device_session.hpp"  // orb::app::DeviceSession
#include "recovery.hpp"        // orb::app::RebootRecovery

namespace orb::app {

class Housekeeping {
   public:
    Housekeeping(DeviceSession& device_session, RebootRecovery& reboot_recovery)
        : device_session_(device_session), reboot_recovery_(reboot_recovery) {}

    Housekeeping(const Housekeeping&) = delete;
    Housekeeping& operator=(const Housekeeping&) = delete;

    // The core0 housekeeping task body (was housekeeping_task). Never returns.
    void run();

   private:
    DeviceSession& device_session_;
    RebootRecovery& reboot_recovery_;
};

}  // namespace orb::app
