#include <device/usbd.h>
#include <hardware/clocks.h>
#include <algorithm>
#include <pico/multicore.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <utility>  // std::to_underlying

#include "FreeRTOS.h"
#include "task.h"

#include "app_tasks.h"

#include "core/section.hpp"
#include "hal/platform.hpp"

#include "adapter.h"
#include "app_queues.h"
#include "adapter_ctx.h"
#include "hardware/structs/vreg_and_chip_reset.h"
#include "hardware/structs/watchdog.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "instrument_manager.h"
#include "midi.h"
#include "orb_bsp.h"
#include "orb_log.h"
// packet_queue.h is a C++ header now (plain C++ free functions). The xbox device driver
// seam (xboxd_* / the weak *_cb hooks) moved to modules/app/device_session.cpp along with
// the device-RX state machine that implements it; the host-side counterpart
// (xbox_controller_driver.h) moved to modules/app/host_controller.cpp along with the
// xboxh_* seam it implements.
#include "packet_queue.h"
#include "system.hpp"

// Board GPIO actuation (LED / hub RESET# / 5V enable) now lives in orb::board::Actuators,
// owned by orb::app::System and reached via orb::app::system().actuators(). See
// modules/board/actuators.{hpp,cpp}.

// Cross-core adapter state (state, tracked controller, liveness/seen/reinit flags)
// now lives behind the adapter_ctx module -- see inc/adapter_ctx.h for the concurrency
// rationale (packed controller word, lock-free volatiles).

// Device-side scratch packet, the identify-sequence cursor, and the announce heartbeat
// (was out_packet / handle_identify's identify_sequence / announce_task's
// last_announce_time), plus the core0 device-RX state machine (auth / identify / init /
// running) and the USB device task itself, now live in orb::app::DeviceSession
// (modules/app/device_session.{hpp,cpp}), owned by orb::app::System and reached via
// orb::app::system().device_session(). See modules/core/seam_anchor.hpp for how the
// TinyUSB xboxd_* callbacks dispatch into it.

// Host-side scratch packet for the core1 controller-input path, the runtime (non-reboot)
// host recovery state, and the core1 host loop itself now live in orb::app::HostController
// (modules/app/host_controller.{hpp,cpp}), owned by orb::app::System and reached via
// orb::app::system().host_controller(). See modules/core/seam_anchor.hpp for how the
// TinyUSB xboxh_* callbacks dispatch into it.

// tinyusb 0.18's enumeration uses blocking osal_task_delay() (= sleep_ms) on the
// host core. sleep_ms hangs on the core running Pico-PIO-USB (it waits on an
// alarm event that core never receives), which stalls enumeration right after
// attach. Override the weak delay with a timer-based busy-wait that works on
// either core and keeps the PIO SOF interrupt running.
void ORB_FAST(tusb_time_delay_ms_api)(uint32_t ms) {
    // Lock-free busy-wait on the raw timer: time_us_64()/busy_wait()/sleep_ms()
    // take a spin lock / wait on an alarm that hangs on the core running
    // Pico-PIO-USB, so they cannot be used on core1. timerawl is lock-free.
    uint32_t start = timer_hw->timerawl;
    uint32_t us = ms * 1000u;
    while ((uint32_t)(timer_hw->timerawl - start) < us) {
        tight_loop_contents();
    }
}


static void init() {
    // 120 MHz (no overclock): with upstream Pico-PIO-USB (post-0.7.2 bus-turnaround
    // / handshake timing fixes), the controller enumerates reliably through the
    // CH334R repeater at the stock 120 MHz the PIO-USB library is designed for.
    set_sys_clock_khz(120000, true);

    // Composition root: construct orb::app::System (adapter state, device TX fifo,
    // inter-task queues, InstrumentManager, ...) before any forwarder below can reach it.
    orb::app::system_init();

    // Bind the TinyUSB HID/MIDI/host-controller seams (modules/driver/guitar_hid_driver.h,
    // modules/driver/drums_midi_seam.h, modules/app/host_controller.hpp) to this System's
    // GuitarHost/DrumEngine/HostController. Must happen before tuh_init() runs (core1's
    // HostController::configure_host(), launched later from main()) -- see
    // modules/core/seam_anchor.hpp.
    orb::app::bind_usb_seams();

    // Bring the cross-core adapter context up before anything can touch it
    // (state=STATE_NONE, no controller tracked, flags cleared).
    orb::service::adapter().reset();

    // Cross-core queues (host-TX core0->core1, MIDI notes core1->core0). Created
    // before the scheduler starts; safe to create here alongside the other init.
    app_queues_init();

    // orb_log owns the debug UART (uart1, GPIO24/25) via dlog and attaches the
    // USB-stick mirror sink; the logger and the TinyUSB logs both drain through it
    // deferred, so no synchronous stdio UART is set up.
    orb::log::init();
    LOG_INFO(CAT_SYS, "openrb debug console initialized...");

    // Reset-cause instrumentation: what kind of reset brought us here?
    {
        uint32_t cr = vreg_and_chip_reset_hw->chip_reset;
        LOG_INFO(CAT_SYS, "RESET CAUSE: chip_reset=0x%08lx POR=%d RUN=%d PSM=%d wd_reboot=%d",
                 (unsigned long)cr,
                 !!(cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS),
                 !!(cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS),
                 !!(cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS),
                 watchdog_caused_reboot());

        orb::app::system().reboot_recovery().restore_count_from_scratch();
    }

    // The deferred log's USB-stick mirror (usb_log_write sink) was attached by
    // orb_log_init() above: core0 pushes drained bytes into the ring and core1
    // (which owns the USB host stack) writes them out to LOG.TXT on the drive.

    xbox_fifo_init();
    LOG_INFO(CAT_SYS, "finished initializing xbox fifo...");

    // Instrument hot-plug event queue: producers (guitar/drums/midi, either core) post
    // {instrument, connect} events; the core0 instrument_task is the sole applier. Must
    // exist before any connect/disconnect_instrument call (all after the scheduler starts).
    instrument_manager_init();

    // LED: push-pull output, start low (auth not yet established).
    orb::app::system().actuators().init_led();

    // Reset the hub before bringing up the host so a warm/watchdog reset
    // re-enumerates the controller cleanly instead of staying wedged.
    orb::app::system().actuators().reset_usb_hub();

    // The USB host stack now comes up inside usb_host_task on core1 once the
    // FreeRTOS scheduler launches that core (see main()).
    LOG_INFO(CAT_SYS, "starting usb device stack");
    tud_init(TUD_OPT_RHPORT);

    serial_midi_init();
    LOG_INFO(CAT_SYS, "finished initializing serial midi...");

    orb::service::adapter().set_state(adapter_state_t::STATE_INIT);
    LOG_INFO(CAT_SYS, "finished init, starting main process...");
}

int main() {
    init();
    app_start_tasks();      // creates + pins usb_host_task (core1) and the core0 tasks
    vTaskStartScheduler();  // launches core1; never returns
    for (;;) {
    }
}
