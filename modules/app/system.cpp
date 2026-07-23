/*
 * Composition-root bridge TU. Defines the single orb::app::System instance plus the small
 * amount of init-time wiring (system_init(), bind_usb_seams()) that has to run from here --
 * the ONLY place (besides main.cpp) allowed to include system.hpp (service/ TUs never
 * include app/ headers; this is where app-reaching code lives per the layering rule).
 *
 * P1-P4 grew a free-function forwarder per leaf/service object so every existing caller could
 * keep calling a plain function while the callee moved into System. P5 retired the last of
 * those callers (main.cpp's init() and xbox_device_driver.cpp's xboxd_send_task now reach the
 * owned objects directly / through a SeamAnchor), so the forwarders themselves are gone --
 * this file is back down to the composition root proper.
 *
 * std::optional<System> gives System a trivial static-storage-duration slot (BSS, no
 * global ctor); system_init() emplaces it once, after set_sys_clock_khz (see main.cpp's
 * init()) so hardware-touching member ctors run post-clock.
 */
#include "system.hpp"

#include <optional>

#include "drums_midi_seam.h"      // orb::driver::bind_drums_midi
#include "guitar_hid_driver.h"    // orb::driver::bind_guitar_hid
#include "xbox_device_driver.h"   // orb::driver::bind_device_tx_fifo

namespace orb::app {
namespace {
std::optional<System> g_system;
}

System& system() { return *g_system; }
void system_init() { g_system.emplace(); }

void bind_usb_seams() {
    orb::driver::bind_guitar_hid(system().guitars());
    orb::driver::bind_drums_midi(system().drums());
    orb::driver::bind_device_tx_fifo(system().tx_fifo());
    orb::app::bind_host_controller(system().host_controller());
    orb::app::bind_device_session(system().device_session());
}

}  // namespace orb::app
