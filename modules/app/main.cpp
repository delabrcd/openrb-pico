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
#include "hardware/structs/vreg_and_chip_reset.h"
#include "hardware/structs/watchdog.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "orb_bsp.h"
#include "orb_log.h"
#include "system.hpp"  // orb::app::system() — the composition root; the device/host seams
                       // and every service/queue this file touches hang off it.

// tinyusb's enumeration delay (osal_task_delay = sleep_ms) hangs on the core running
// Pico-PIO-USB — it waits on an alarm that core never services. Override it with a
// lock-free busy-wait on the raw timer: safe on either core, keeps the PIO SOF running.
void ORB_FAST(tusb_time_delay_ms_api)(uint32_t ms) {
    uint32_t start = timer_hw->timerawl;
    uint32_t us = ms * 1000u;
    while ((uint32_t)(timer_hw->timerawl - start) < us) {
        tight_loop_contents();
    }
}

static void init() {
    // 120 MHz (stock, no overclock): with upstream Pico-PIO-USB the controller enumerates
    // reliably through the CH334R repeater at the clock the PIO-USB library targets.
    set_sys_clock_khz(120000, true);

    // Composition root, then bind the TinyUSB HID/MIDI/host seams to it — both must precede
    // any seam callback and tuh_init() (core1's HostController, launched from main()).
    orb::app::system_init();
    orb::app::bind_usb_seams();

    orb::app::system().adapter().reset();

    // Cross-core queues (host-TX core0->core1, MIDI notes core1->core0); pre-scheduler.
    orb::app::system().host_tx().create();
    orb::app::system().midi_notes().create();

    // orb_log owns the debug UART (uart1, GPIO24/25) via dlog and attaches the USB-stick
    // mirror sink; logging is deferred, so there's no synchronous stdio UART.
    orb::log::init();
    LOG_INFO(CAT_SYS, "openrb debug console initialized...");

    // What kind of reset brought us here (also restores the reboot-recovery count).
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

    orb::app::system().tx_fifo().init();
    LOG_INFO(CAT_SYS, "finished initializing xbox fifo...");

    // Instrument hot-plug event queue — must exist before any connect/disconnect call.
    orb::app::system().instruments().init_queue();

    orb::app::system().actuators().init_led();  // push-pull, starts low (unauthenticated)

    // Reset the hub before host bring-up so a warm/watchdog reset re-enumerates the
    // controller cleanly instead of staying wedged.
    orb::app::system().actuators().reset_usb_hub();

    // The USB host stack comes up in usb_host_task on core1 once the scheduler launches it.
    LOG_INFO(CAT_SYS, "starting usb device stack");
    tud_init(TUD_OPT_RHPORT);

    orb::app::system().serial_midi().init();
    LOG_INFO(CAT_SYS, "finished initializing serial midi...");

    orb::app::system().adapter().set_state(adapter_state_t::STATE_INIT);
    LOG_INFO(CAT_SYS, "finished init, starting main process...");
}

int main() {
    init();
    app_start_tasks();      // creates + pins usb_host_task (core1) and the core0 tasks
    vTaskStartScheduler();  // launches core1; never returns
    for (;;) {
    }
}
