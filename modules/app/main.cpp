#include <bsp/board_api.h>
#include <device/usbd.h>
#include <hardware/clocks.h>
#include <algorithm>
#include <pico/multicore.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <chrono>
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
#include "osal/task.hpp"

#include "adapter.h"
#include "app_queues.h"
#include "adapter_ctx.h"
#include "dlog.h"
#include "drums.h"
#include "hardware/structs/vreg_and_chip_reset.h"
#include "hardware/structs/watchdog.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "identifiers.h"
#include "instrument_manager.h"
#include "midi.h"
#include "orb_bsp.h"
#include "orb_log.h"
#include "usb_log.h"
// packet_queue.h is a C++ header now (plain C++ free functions). The xbox device driver
// header is a C++ TU but exposes the genuine TinyUSB driver-class callback seam (xboxd_* /
// the weak *_cb hooks implemented below), so it keeps its own internal extern "C" guard --
// include it normally here. The host-side counterpart (xbox_controller_driver.h) moved to
// modules/app/host_controller.cpp along with the xboxh_* seam it implements.
#include "packet_queue.h"
#include "system.hpp"
#include "xbox_device_driver.h"

// Board GPIO actuation (LED / hub RESET# / 5V enable) now lives in orb::board::Actuators,
// owned by orb::app::System and reached via orb::app::system().actuators(). See
// modules/board/actuators.{hpp,cpp}.

// Cross-core adapter state (state, tracked controller, liveness/seen/reinit flags)
// now lives behind the adapter_ctx module -- see inc/adapter_ctx.h for the concurrency
// rationale (packed controller word, lock-free volatiles).

// Device-side scratch packet: built by the core0 device-RX handlers (auth / identify /
// running / announce) before being copied into the cross-core TX fifo. core0 ONLY.
static XboxPacket out_packet;

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


void xboxd_on_reset_cb() {
    orb::app::system().actuators().set_auth_led(false);

    // TODO CDD - look into a better way of reinitializing the USB Host stack than a hard reset
    if (orb::service::adapter().state() != adapter_state_t::STATE_INIT &&
        orb::service::adapter().state() != adapter_state_t::STATE_NONE)
        watchdog_reboot(0, 0, 10);
}

static void handle_auth(const XboxPacket *packet) {
    if (packet->frame().command == frame_command_e::CMD_AUTHENTICATE &&
        packet->frame().length == 2 &&
        packet->data()[3] == 2 && packet->data()[4] == 1 && packet->data()[5] == 0) {
        orb::app::system().actuators().set_auth_led(true);

        LOG_INFO(CAT_DEV, "AUTHENTICATED!");
        orb::service::adapter().set_state(adapter_state_t::STATE_RUNNING);

        notify_xbox_of_all_instruments(out_packet);
    }

    LOG_DBG(CAT_DEV, "Sending controller %d bytes", packet->length);
    host_tx_send(packet);  // drained + sent on core1 by HostController::run
    return;
}

static void handle_identify(const XboxPacket *packet) {
    static uint8_t identify_sequence = 0;
    switch (packet->frame().command) {
        case frame_command_e::CMD_IDENTIFY:
        case frame_command_e::CMD_ACKNOWLEDGE:
            if (identify_sequence >= identifiers_get_n()) {
                LOG_INFO(CAT_DEV, "Starting identify sequence over");
                identify_sequence = 0;
            }
            identifiers_get(identify_sequence, &out_packet);
            xbox_fifo_write(&out_packet);
            identify_sequence++;
            break;
        case frame_command_e::CMD_AUTHENTICATE:
            LOG_INFO(CAT_DEV, "Moving to Authenticate");
            orb::service::adapter().set_state(adapter_state_t::STATE_AUTHENTICATING);
            return handle_auth(packet);
            break;
        default:
            break;
    }
    return;
}

static void handle_init(const XboxPacket *packet) {
    switch (packet->frame().command) {
        case frame_command_e::CMD_IDENTIFY:
            LOG_INFO(CAT_DEV, "Moving to Identify");
            orb::service::adapter().set_state(adapter_state_t::STATE_IDENTIFYING);
            return handle_identify(packet);
        default:
            break;
    }
}

static void handle_running(const XboxPacket *packet) {
    switch (packet->frame().command) {
        case frame_command_e::CMD_POWER_MODE:
            if (packet->power().data == std::to_underlying(power_mode_e::POWER_OFF)) {
                orb::service::adapter().set_state(adapter_state_t::STATE_POWER_OFF);
                orb::app::system().actuators().set_auth_led(false);
                orb::app::system().actuators().set_usb_host(false);
            }
            break;
        case frame_command_e::CMD_ACKNOWLEDGE:
            host_tx_send(packet);  // drained + sent on core1 by HostController::run
            break;
        case frame_command_e::CMD_LIST_CONNECTED_INSTRUMENTS:
            notify_xbox_of_all_instruments(out_packet);
            break;
        case frame_command_e::CMD_LIST_INSTRUMENT:
            notify_xbox_of_single_instrument(static_cast<instruments_e>(packet->data()[4]), out_packet);
            break;
        default:
            break;
    }
    return;
}

static void handle_xboxd_packet(const XboxPacket *packet) {
    switch (orb::service::adapter().state()) {
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

bool xboxd_packet_received_cb(uint8_t rhport, const XboxPacket *buf, uint32_t xferred_bytes) {
    (void)rhport;
    if (xferred_bytes < sizeof(frame_t)) return false;

    handle_xboxd_packet(buf);
    return true;
}

static void announce_task() {
    if (orb::service::adapter().state() != adapter_state_t::STATE_INIT) return;

    static unsigned long last_announce_time = 0;
    if (std::chrono::milliseconds(board_millis() - last_announce_time) > orb::service::announce_interval) {
        if (orb::service::adapter().controller_idx() < UINT8_MAX) {
            LOG_INFO(CAT_DEV, "ANNOUNCING");
            identifiers_get_announce(&out_packet);
            xbox_fifo_write(&out_packet);
            last_announce_time = board_millis();
        }
    }
}

// USB host task. Pinned to core1 and the ONLY task that runs there, so the
// PIO-USB bit-banged signalling sees ~no FreeRTOS context switches. The SMP
// scheduler launches core1 itself in vTaskStartScheduler(), so there is no more
// multicore_launch_core1()/launch_core1_robust() (and thus no early-launch FIFO
// handshake race -- ROOT CAUSE #1 is now owned by the FreeRTOS port). Body now lives in
// orb::app::HostController::run() (modules/app/host_controller.cpp) -- this is a thin
// trampoline so app_tasks.cpp's task registration stays unchanged.
void usb_host_task(void *param) {
    (void)param;
    orb::app::system().host_controller().run();
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

    std::ranges::fill(out_packet.wire(), std::uint8_t{0});

    orb::service::adapter().set_state(adapter_state_t::STATE_INIT);
    LOG_INFO(CAT_SYS, "finished init, starting main process...");
}

// ---- core0 per-concern tasks (all pinned to core0; see app_tasks.cpp) -----------

// USB device stack + the send drain. These stay in ONE task because both touch the
// device endpoint (tud_task processes events; xboxd_send_task claims the IN endpoint),
// and TinyUSB device-stack access must be serialized. tud_task_ext(4) blocks on the
// device event queue but wakes at least every 4ms to drain device_tx to the console.
void usb_device_task(void *param) {
    (void)param;
    while (true) {
        tud_task_ext(4, false);
        xboxd_send_task();
    }
}

// Instrument input: drains the USB-MIDI note queue (filled on core1) + serial MIDI,
// ages out hits, and writes drum packets to the device fifo. ~2ms cadence matches the
// drum trigger/output timing.
void drum_input_task(void *param) {
    (void)param;
    while (true) {
        drum_task();
        orb::osal::sleep_for(std::chrono::milliseconds(2));
    }
}

// Instrument hot-plug owner (core0): the ONLY mutator of the connection state and the only
// place the add/drop packet is built + queued to the device. Blocks on the instrument event
// queue, so it costs nothing until a guitar/drum mounts or umounts. Keeping this off core1
// is the whole point -- the producers (USB umount callbacks) just post and return.
void instrument_task(void *param) {
    (void)param;
    while (true) {
        instrument_manager_service();  // parks until an event arrives, then applies it
    }
}

// Low-priority periodic background: controller-announce heartbeat (gated to STATE_INIT,
// fires ~every 2s internally), warm-reset zombie recovery, and draining the deferred
// log to UART/USB. Grouped because all three are coarse periodic chores.
void housekeeping_task(void *param) {
    (void)param;
    while (true) {
        announce_task();
        orb::app::system().reboot_recovery().service();
        dlog_drain();
        orb::osal::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    init();
    app_start_tasks();      // creates + pins usb_host_task (core1) and the core0 tasks
    vTaskStartScheduler();  // launches core1; never returns
    for (;;) {
    }
}
