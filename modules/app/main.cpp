#include <bsp/board_api.h>
#include <device/usbd.h>
#include <hardware/clocks.h>
#include <host/usbh.h>
#include <algorithm>
#include <pico/multicore.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <atomic>
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
#include "hardware/dma.h"
#include "hardware/structs/vreg_and_chip_reset.h"
#include "hardware/structs/watchdog.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "identifiers.h"
#include "instrument_manager.h"
#include "midi.h"
#include "orb_bsp.h"
#include "orb_log.h"
#include "pio_usb_configuration.h"
#include "usb_log.h"
// packet_queue.h is a C++ header now (plain C++ free functions). The xbox host/device
// driver headers are C++ TUs but expose the genuine TinyUSB driver-class callback seam
// (xboxh_* / xboxd_* / the weak *_cb hooks implemented below), so they keep their own
// internal extern "C" guard -- include all three normally here.
#include "packet_queue.h"
#include "system.hpp"
#include "xbox_controller_driver.h"
#include "xbox_device_driver.h"
#include "drums_midi_seam.h"  // drums_read_midi_host (moved out of drums.h -- driver seam TU)

inline constexpr uint8_t kHostControllerId = 1u;
// FIRST_XBOX_CONTROLLER_IDX removed -- unused constant.

// Board GPIO actuation (LED / hub RESET# / 5V enable) now lives in orb::board::Actuators,
// owned by orb::app::System and reached via orb::app::system().actuators(). See
// modules/board/actuators.{hpp,cpp}.

// Cross-core adapter state (state, tracked controller, liveness/seen/reinit flags)
// now lives behind the adapter_ctx module -- see inc/adapter_ctx.h for the concurrency
// rationale (packed controller word, lock-free volatiles).

// Device-side scratch packet: built by the core0 device-RX handlers (auth / identify /
// running / announce) before being copied into the cross-core TX fifo. core0 ONLY.
static XboxPacket out_packet;

// Host-side scratch packet for the core1 controller-input path (handle_controller_packet_
// running). core1 ONLY -- kept separate from out_packet so a core0 device-RX handler building
// out_packet cannot tear a controller-input packet being built concurrently on core1 (both
// are full-width rebuilt-then-enqueued; only the fifo copy is mutex-protected, not the build).
static XboxPacket host_out_packet;

// ---- Runtime (non-reboot) host recovery, core1 ---------------------------------
// Liveness of the tracked controller, sampled on core1 only (mount_cb /
// packet_received_cb / usb_host_task all run on core1, so plain statics are safe --
// no cross-core sync needed). g_host_last_rx_us is a lock-free timestamp of the last
// real input (timer_hw->timerawl -- board_millis()/sleep hang core1, see usb_host_task);
// g_host_rx_count ticks on every received packet so the recovery loop can tell a fresh
// heartbeat (refill the attempt budget) from us merely bumping the silence timer.
static constexpr std::memory_order kRlx = std::memory_order_relaxed;
static std::atomic<uint32_t> g_host_last_rx_us{0};
// g_host_rx_count: written core1 (xboxh_packet_received_cb), read core1
// (host_recovery_task) -- single-core but atomic for symmetric treatment.
static std::atomic<uint32_t> g_host_rx_count{0};

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

static inline bool xboxh_send(const XboxPacket *buffer) {
    return host_tx_send(buffer);  // drained + sent on core1 by usb_host_task
}

void xboxh_mount_cb(uint8_t dev_addr, uint8_t instance) {
    LOG_INFO(CAT_HOST, "Controller %d Connected", instance);
    orb::service::adapter().set_seen(true);  // a controller mounted this boot (may still be a zombie)
    // Always follow the most-recently-connected controller. On a replug the device
    // gets a new instance/address (and, if the old umount is missed or races, the
    // stale slot can linger), so adopting only when idx==UINT8_MAX would leave us
    // forwarding from the wrong slot -- the controller re-enumerates (LED on) but
    // its inputs get filtered by xboxh_packet_received_cb. We only track one active
    // controller, so taking over on every mount is correct and replug-safe.
    orb::service::adapter().set_controller(instance, dev_addr);
    // Grant a fresh silence window so the runtime recovery (usb_host_task) doesn't
    // mistake the gap between mount and the first input report for a wedged hub.
    g_host_last_rx_us.store(timer_hw->timerawl, kRlx);
}

void xboxh_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    LOG_INFO(CAT_HOST, "Controller %d Disconnected", instance);
    if (instance == orb::service::adapter().controller_idx()) {
        orb::service::adapter().clear_controller(instance);
        orb::service::adapter().set_alive(false);  // require a fresh heartbeat from the next mount
    }
}

void handle_controller_packet_running(const XboxPacket *data) {
    switch (data->frame().command) {
        case frame_command_e::CMD_ANNOUNCE:
            // Controller re-attached and is announcing -- it won't stream input until
            // the host re-inits it. Defer to the core1 loop (the init blocks on tx).
            orb::service::adapter().request_reinit();
            break;

        case frame_command_e::CMD_GUIDE_BTN:
            xbox_fifo_write(data);
            break;

        case frame_command_e::CMD_INPUT:
            fill_drum_input_from_controller(data, &host_out_packet,
                                            std::to_underlying(instruments_e::DRUMS));
            xbox_fifo_write(&host_out_packet);
            break;
        default:
            break;
    }
}

void xboxh_packet_received_cb(uint8_t idx, const XboxPacket *data, const uint8_t ndata) {
    if (idx != orb::service::adapter().controller_idx()) return;
    if (ndata < sizeof(frame_t)) return;
    orb::service::adapter().set_alive(true);  // a real packet arrived -> controller is alive, not a zombie
    g_host_last_rx_us.store(timer_hw->timerawl, kRlx);  // feed the runtime-recovery silence timer (core1)
    g_host_rx_count.store(g_host_rx_count.load(kRlx) + 1u, kRlx);  // tick so recovery can detect a fresh heartbeat
    LOG_TRC(CAT_WIRE, "IN FROM CONTROLLER: %s", get_command_name(std::to_underlying(data->frame().command)));
    switch (orb::service::adapter().state()) {
        case adapter_state_t::STATE_AUTHENTICATING:
            xbox_fifo_write(data);
            break;
        // case adapter_state_t::STATE_POWER_OFF:
        //     break;
        case adapter_state_t::STATE_RUNNING:
            handle_controller_packet_running(data);
            break;
        default:
            break;
    }
}

void xboxh_packet_sent_cb(uint8_t idx, const XboxPacket *data, const uint8_t ndata) {
    (void)idx;
    (void)data;
    (void)ndata;
    LOG_TRC(CAT_WIRE, "Sent Controller %d bytes (%s)", ndata, get_command_name(std::to_underlying(data->frame().command)));
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
    xboxh_send(packet);
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
            xboxh_send(packet);
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

static void configure_host() {
    LOG_INFO(CAT_HOST, "configuring usb host stack");

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = orb::board::pin_usb_host_dp;

    // find an unused channel
    pio_cfg.tx_ch = dma_claim_unused_channel(true);
    dma_channel_unclaim(pio_cfg.tx_ch);
    tuh_configure(kHostControllerId, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(kHostControllerId);
    LOG_INFO(CAT_HOST, "finished configuring usb host");

    orb::app::system().actuators().set_usb_host(true);
}

// Runtime (no-reboot) host recovery, run from the core1 host loop.
//
// ROOT CAUSE: the Xbox controller sits one repeater hop behind the CH334R hub, so
// TinyUSB only learns it dropped via the hub's interrupt-IN port-status-change
// reports. When SOF stops (debugger halts both cores, or any transient glitch) the
// CH334R wedges: it stops reporting port changes AND keeps its upstream D+ pull-up
// asserted, so the root port never goes SE0. Result: TinyUSB sees neither a hub
// disconnect nor a downstream port change -- the controller stays "mounted" but its
// interrupt-IN transfers quietly error/time out (xboxh_xfer_cb ignores result and
// just re-arms), so it lingers as a silent zombie that never re-enumerates.
//
// FIX: detect the silence on core1 and pulse the hub's active-low RESET# (the exact
// boot-time reset_usb_hub() recovery). Asserting RESET# drops the hub's upstream
// pull-up -> the root port finally sees SE0 -> Pico-PIO-USB raises a disconnect ->
// TinyUSB's process_removed_device() tears down the hub and, recursively, every
// downstream device (the controller umounts cleanly), then re-enumerates the whole
// tree on release. No host re-init / tuh_deinit needed -- we lean on the normal,
// well-tested attach/detach path; only the wedged silicon needs the RESET# kick.
//
// This is the PREFERRED recovery (works pre- AND post-auth, no chip reboot);
// RebootRecovery::service (core0, see recovery.hpp) defers to it via RecoveryState and only
// watchdog-reboots if this gives up. Custom board only -- FEATHER has no hub.
#if ORB_BOARD_ID == ORB_BOARD_ID_CUSTOM_REV_0_1
// "Wedged" is detected two ways (measured on hardware):
//  1. FAST -- a wedged CH334R fails the controller's interrupt-IN poll continuously
//     (~80 failures/s), while a healthy idle pad produces NO completions between its
//     sparse packets (NAKs don't complete). So a run of consecutive IN failures is a
//     ~1 s, false-positive-free wedge signal (xboxh_in_error_streak()).
//  2. BACKSTOP -- in case a wedge ever stops the IN poll entirely instead of failing
//     it, also trigger on prolonged silence. The threshold must exceed the pad's idle
//     CMD_STATUS keep-alive (~20 s, which refreshes g_host_last_rx_us), so 30 s clears
//     the heartbeat with margin. In active play input streams sub-second, so neither
//     path fires spuriously there.
inline constexpr uint32_t kHostRecovErrStreak = 100u;     // ~1.25 s of continuous IN failures -> wedged (fast)
inline constexpr uint32_t kHostRecovSilenceUs = 30000000u; // no packet at all this long -> wedged (backstop)
inline constexpr uint32_t kHostRecovGraceUs = 3000000u;    // wait this long between resets for re-enumeration
inline constexpr uint8_t  kHostRecovMax = 3u;              // bounded so a genuine unplug can't thrash the hub forever
static void host_recovery_task(void) {
    static bool recovering = false;       // currently trying to get a lost controller back
    static bool gave_up = false;          // exhausted attempts; wait for real input before re-arming
    static uint8_t attempts = 0;
    static uint32_t last_attempt_us = 0;
    static uint32_t last_rx_count = 0;

    uint32_t now = timer_hw->timerawl;

    // A fresh heartbeat means a healthy controller: exit recovery and refill the budget.
    uint32_t rxc = g_host_rx_count.load(kRlx);
    if (rxc != last_rx_count) {
        last_rx_count = rxc;
        recovering = false;
        gave_up = false;
        attempts = 0;
    }

    bool wedged = xboxh_in_error_streak() >= kHostRecovErrStreak ||
                  (uint32_t)(now - g_host_last_rx_us.load(kRlx)) > kHostRecovSilenceUs;

    // Detect a loss: a controller mounted this boot but has since gone wedged. A clean
    // unplug also looks like this, so attempts are bounded (gave_up) until input returns.
    if (!recovering && !gave_up && orb::service::adapter().seen() && wedged) {
        recovering = true;
        attempts = 0;
        last_attempt_us = now - kHostRecovGraceUs;  // act on the first pass
    }

    if (recovering) {
        if (attempts >= kHostRecovMax) {
            // Couldn't bring it back -> stand down and let the reboot path (last resort)
            // decide. Stay quiet until a real packet refills the budget (got input above).
            recovering = false;
            gave_up = true;
        } else if ((uint32_t)(now - last_attempt_us) >= kHostRecovGraceUs) {
            attempts++;
            last_attempt_us = now;
            g_host_last_rx_us.store(now, kRlx);  // suppress the silence backstop during re-enumeration
            xboxh_clear_error_streak();    // fresh count; GRACE lets re-enum land before it re-trips
            LOG_WARN(CAT_RECOV, "HOST RECOVERY: controller wedged -> hub reset %u/%u",
                     (unsigned)attempts, (unsigned)kHostRecovMax);
            orb::app::system().actuators().reset_usb_hub();  // RESET# pulse (busy_wait_ms, core1-safe); TinyUSB re-enumerates
        }
    }

    orb::app::system().recovery_state().set_engaged(recovering);  // gate the core0 watchdog-reboot path
}
#else
static inline void host_recovery_task(void) {}  // no hub / no RESET# pin on this board
#endif

// USB host task. Pinned to core1 and the ONLY task that runs there, so the
// PIO-USB bit-banged signalling sees ~no FreeRTOS context switches. The SMP
// scheduler launches core1 itself in vTaskStartScheduler(), so there is no more
// multicore_launch_core1()/launch_core1_robust() (and thus no early-launch FIFO
// handshake race -- ROOT CAUSE #1 is now owned by the FreeRTOS port).
void usb_host_task(void *param) {
    (void)param;
    // Settle before bringing up PIO-USB (matches the Pico-PIO-USB examples'
    // sleep_ms(10)); lock-free timer wait to avoid any alarm-pool dependency.
    uint32_t t = timer_hw->timerawl;
    while ((uint32_t)(timer_hw->timerawl - t) < 10000u) tight_loop_contents();
    configure_host();
    uint32_t last_reinit_us = 0;  // lock-free timer (timerawl) -- board_millis() spinlock hangs core1
    while (true) {
        // tuh_task_ext(10): block on the host event queue but wake at least every 10ms
        // so the reinit/host-tx/midi/usb_log/recovery work below still runs when the bus
        // is idle. (Under OPT_OS_FREERTOS a plain tuh_task() blocks forever.)
        tuh_task_ext(10, false);
        // A running controller that re-announced needs its init re-sent (Issue: a
        // post-auth replug leaves it announcing forever, never streaming input).
        // Debounce so we re-init at most ~2x/s instead of on every announce.
        if (orb::service::adapter().take_reinit()) {
            uint32_t now = timer_hw->timerawl;
            uint8_t idx, addr;
            if (orb::service::adapter().controller(&idx, &addr) &&
                (uint32_t)(now - last_reinit_us) > 500000u) {
                last_reinit_us = now;
                LOG_WARN(CAT_RECOV, "Controller re-announced -> re-init");
                xboxh_reinit_controller(addr, idx);
            }
        }

        host_recovery_task();

        // Drain host-TX packets enqueued by core0 device handlers; submit on core1 (the
        // core that owns the host stack).
        XboxPacket txp;
        while (host_tx_recv(&txp)) {
            uint8_t idx, addr;
            if (orb::service::adapter().controller(&idx, &addr)) {
                xboxh_send_report(addr, idx, &txp, txp.length);
            }
        }

        // Read USB-host MIDI on core1 and hand parsed notes to core0 via the queue.
        drums_read_midi_host();

        usb_log_task();        // drain the log ring out to the USB flash drive
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

    // Bind the TinyUSB HID/MIDI seams (modules/driver/guitar_hid_driver.h,
    // modules/driver/drums_midi_seam.h) to this System's GuitarHost/DrumEngine. Must happen
    // before tuh_init() runs (core1's configure_host(), launched later from main()) -- see
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
