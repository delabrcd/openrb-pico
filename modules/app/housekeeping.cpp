/*
 * orb::app::Housekeeping -- see housekeeping.hpp for the rationale. run() is the former
 * main.cpp housekeeping_task loop, moved verbatim.
 */
#include "housekeeping.hpp"

#include <chrono>

#include "dlog.h"          // dlog_drain
#include "osal/task.hpp"   // orb::osal::sleep_for

namespace orb::app {

void Housekeeping::run() {
    while (true) {
        device_session_.announce();
        reboot_recovery_.service();
        dlog_drain();
        orb::osal::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace orb::app
