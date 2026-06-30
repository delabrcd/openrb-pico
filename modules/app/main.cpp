#include <bsp/board_api.h>
#include <device/usbd.h>
#include <hardware/clocks.h>
#include <host/usbh.h>
#include <optional>
#include <pico/multicore.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>  // std::to_underlying

#include "FreeRTOS.h"
#include "task.h"

#include "app_tasks.h"

#include "core/section.hpp"
#include "hal/platform.hpp"

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
#include "xbox_controller_driver.h"
#include "xbox_device_driver.h"

#define HOST_CONTROLLER_ID 1
#define FIRST_XBOX_CONTROLLER_IDX 0

// GPIO hardware objects — emplaced at init time (not as globals) per the hardware-object
// lifetime rule in docs/architecture/modern-cpp.md: constructors that touch the chip must
// not run before main() sets the system clock. std::optional has a trivial constructor so
// these land in BSS with no global-ctor.
static std::optional<orb::hal::GpioOut> s_led;         // auth/status LED
static std::optional<orb::hal::GpioOd>  s_hub_rst;     // CH334R RESET# (CUSTOM board only)
static std::optional<orb::hal::GpioOut> s_5v_en;       // 5V enable (FEATHER board only)

// Cross-core adapter state (state, tracked controller, liveness/seen/reinit flags)
// now lives behind the adapter_ctx module -- see inc/adapter_ctx.h for the concurrency
// rationale (packed controller word, lock-free volatiles).

// Device-side scratch packet: built by the core0 device-RX handlers (auth / identify /
// running / announce) before being copied into the cross-core TX fifo. core0 ONLY.
static xbox_packet_t out_packet;

// Host-side scratch packet for the core1 controller-input path (handle_controller_packet_
// running). core1 ONLY -- kept separate from out_packet so a core0 device-RX handler building
// out_packet cannot tear a controller-input packet being built concurrently on core1 (both
// are full-width rebuilt-then-enqueued; only the fifo copy is mutex-protected, not the build).
static xbox_packet_t host_out_packet;

// ---- Runtime (non-reboot) host recovery, core1 ---------------------------------
// Liveness of the tracked controller, sampled on core1 only (mount_cb /
// packet_received_cb / usb_host_task all run on core1, so plain statics are safe --
// no cross-core sync needed). g_host_last_rx_us is a lock-free timestamp of the last
// real input (timer_hw->timerawl -- board_millis()/sleep hang core1, see usb_host_task);
// g_host_rx_count ticks on every received packet so the recovery loop can tell a fresh
// heartbeat (refill the attempt budget) from us merely bumping the silence timer.
static volatile uint32_t g_host_last_rx_us = 0;
static volatile uint32_t g_host_rx_count = 0;
// Set by the core1 runtime recovery while it is actively trying to bring a lost
// controller back via hub RESET#; read by recovery_reboot_task (core0) so the
// watchdog-reboot path defers to this non-reboot recovery and only fires once it
// gives up. Single volatile bool -> lock-free on RP2040. False on boards without a
// hub-reset pin (FEATHER), leaving the reboot path's behaviour unchanged there.
static volatile bool g_runtime_recovery_engaged = false;

// Defined further down (init-time hub reset); reused at runtime by usb_host_task.
static void reset_usb_hub(void);

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


static inline void set_auth_led(bool val) { if (s_led) s_led->set(val); }

static inline void set_usb_host(bool on) {
#if ORB_BOARD_ID == ORB_BOARD_ID_FEATHER
    if (!s_5v_en) s_5v_en.emplace(orb::board::pin_5v_en);
    s_5v_en->set(on);
#else
    (void)on;
#endif
}

void xboxd_on_reset_cb() {
    set_auth_led(false);

    // TODO CDD - look into a better way of reinitializing the USB Host stack than a hard reset
    if (orb::service::adapter().state() != adapter_state_t::STATE_INIT &&
        orb::service::adapter().state() != adapter_state_t::STATE_NONE)
        watchdog_reboot(0, 0, 10);
}

static inline bool xboxh_send(const xbox_packet_t *buffer) {
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
    g_host_last_rx_us = timer_hw->timerawl;
}

void xboxh_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    LOG_INFO(CAT_HOST, "Controller %d Disconnected", instance);
    if (instance == orb::service::adapter().controller_idx()) {
        orb::service::adapter().clear_controller(instance);
        orb::service::adapter().set_alive(false);  // require a fresh heartbeat from the next mount
    }
}

void handle_controller_packet_running(const xbox_packet_t *data) {
    switch (data->frame.command) {
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

void xboxh_packet_received_cb(uint8_t idx, const xbox_packet_t *data, const uint8_t ndata) {
    if (idx != orb::service::adapter().controller_idx()) return;
    if (ndata < sizeof(frame_t)) return;
    orb::service::adapter().set_alive(true);  // a real packet arrived -> controller is alive, not a zombie
    g_host_last_rx_us = timer_hw->timerawl;  // feed the runtime-recovery silence timer (core1)
    g_host_rx_count++;                       // tick so recovery can detect a fresh heartbeat
    LOG_TRC(CAT_WIRE, "IN FROM CONTROLLER: %s", get_command_name(std::to_underlying(data->frame.command)));
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

void xboxh_packet_sent_cb(uint8_t idx, const xbox_packet_t *data, const uint8_t ndata) {
    (void)idx;
    (void)data;
    (void)ndata;
    LOG_TRC(CAT_WIRE, "Sent Controller %d bytes (%s)", ndata, get_command_name(std::to_underlying(data->frame.command)));
}

static void handle_auth(const xbox_packet_t *packet) {
    if (packet->frame.command == frame_command_e::CMD_AUTHENTICATE &&
        packet->frame.length == 2 &&
        packet->buffer[3] == 2 && packet->buffer[4] == 1 && packet->buffer[5] == 0) {
        set_auth_led(true);

        LOG_INFO(CAT_DEV, "AUTHENTICATED!");
        orb::service::adapter().set_state(adapter_state_t::STATE_RUNNING);

        notify_xbox_of_all_instruments(&out_packet);
    }

    LOG_DBG(CAT_DEV, "Sending controller %d bytes", packet->length);
    xboxh_send(packet);
    return;
}

static void handle_identify(const xbox_packet_t *packet) {
    static uint8_t identify_sequence = 0;
    switch (packet->frame.command) {
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

static void handle_init(const xbox_packet_t *packet) {
    switch (packet->frame.command) {
        case frame_command_e::CMD_IDENTIFY:
            LOG_INFO(CAT_DEV, "Moving to Identify");
            orb::service::adapter().set_state(adapter_state_t::STATE_IDENTIFYING);
            return handle_identify(packet);
        default:
            break;
    }
}

static void handle_running(const xbox_packet_t *packet) {
    switch (packet->frame.command) {
        case frame_command_e::CMD_POWER_MODE:
            if (packet->power.data.data == std::to_underlying(power_mode_e::POWER_OFF)) {
                orb::service::adapter().set_state(adapter_state_t::STATE_POWER_OFF);
                set_auth_led(false);
                set_usb_host(false);
            }
            break;
        case frame_command_e::CMD_ACKNOWLEDGE:
            xboxh_send(packet);
            break;
        case frame_command_e::CMD_LIST_CONNECTED_INSTRUMENTS:
            notify_xbox_of_all_instruments(&out_packet);
            break;
        case frame_command_e::CMD_LIST_INSTRUMENT:
            notify_xbox_of_single_instrument(static_cast<instruments_e>(packet->buffer[4]), &out_packet);
            break;
        default:
            break;
    }
    return;
}

static void handle_xboxd_packet(const xbox_packet_t *packet) {
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

bool xboxd_packet_received_cb(uint8_t rhport, const xbox_packet_t *buf, uint32_t xferred_bytes) {
    (void)rhport;
    if (xferred_bytes < sizeof(frame_t)) return false;

    handle_xboxd_packet(buf);
    return true;
}

static void announce_task() {
    if (orb::service::adapter().state() != adapter_state_t::STATE_INIT) return;

    static unsigned long last_announce_time = 0;
    if ((board_millis() - last_announce_time) > orb::service::announce_interval_ms) {
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
    tuh_configure(HOST_CONTROLLER_ID, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(HOST_CONTROLLER_ID);
    LOG_INFO(CAT_HOST, "finished configuring usb host");

    set_usb_host(true);
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
// recovery_reboot_task (core0) defers to it via g_runtime_recovery_engaged and only
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
#define HOST_RECOV_ERR_STREAK 100u       // ~1.25 s of continuous IN failures -> wedged (fast)
#define HOST_RECOV_SILENCE_US 30000000u  // no packet at all this long -> wedged (backstop)
#define HOST_RECOV_GRACE_US 3000000u     // wait this long between resets for re-enumeration
#define HOST_RECOV_MAX 3u                // bounded so a genuine unplug can't thrash the hub forever
static void host_recovery_task(void) {
    static bool recovering = false;       // currently trying to get a lost controller back
    static bool gave_up = false;          // exhausted attempts; wait for real input before re-arming
    static uint8_t attempts = 0;
    static uint32_t last_attempt_us = 0;
    static uint32_t last_rx_count = 0;

    uint32_t now = timer_hw->timerawl;

    // A fresh heartbeat means a healthy controller: exit recovery and refill the budget.
    uint32_t rxc = g_host_rx_count;
    if (rxc != last_rx_count) {
        last_rx_count = rxc;
        recovering = false;
        gave_up = false;
        attempts = 0;
    }

    bool wedged = xboxh_in_error_streak() >= HOST_RECOV_ERR_STREAK ||
                  (uint32_t)(now - g_host_last_rx_us) > HOST_RECOV_SILENCE_US;

    // Detect a loss: a controller mounted this boot but has since gone wedged. A clean
    // unplug also looks like this, so attempts are bounded (gave_up) until input returns.
    if (!recovering && !gave_up && orb::service::adapter().seen() && wedged) {
        recovering = true;
        attempts = 0;
        last_attempt_us = now - HOST_RECOV_GRACE_US;  // act on the first pass
    }

    if (recovering) {
        if (attempts >= HOST_RECOV_MAX) {
            // Couldn't bring it back -> stand down and let the reboot path (last resort)
            // decide. Stay quiet until a real packet refills the budget (got input above).
            recovering = false;
            gave_up = true;
        } else if ((uint32_t)(now - last_attempt_us) >= HOST_RECOV_GRACE_US) {
            attempts++;
            last_attempt_us = now;
            g_host_last_rx_us = now;       // suppress the silence backstop during re-enumeration
            xboxh_clear_error_streak();    // fresh count; GRACE lets re-enum land before it re-trips
            LOG_WARN(CAT_RECOV, "HOST RECOVERY: controller wedged -> hub reset %u/%u",
                     (unsigned)attempts, (unsigned)HOST_RECOV_MAX);
            reset_usb_hub();  // RESET# pulse (busy_wait_ms, core1-safe); TinyUSB re-enumerates
        }
    }

    g_runtime_recovery_engaged = recovering;  // gate the core0 watchdog-reboot path
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
        xbox_packet_t txp;
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

// The CH334R hub stays powered across an RP2040 warm/watchdog reset, so it keeps
// stale state and the downstream controller fails to re-enumerate -- only a cold
// power-on recovers it. GPIO18 (USB_HUB_RST) drives the hub's active-low RESET#;
// pulse it on every boot to force a clean hub reset. Custom board only -- on the
// FEATHER board GPIO18 is the 5V enable.
//
// CH334/335 datasheet (V2.5, sec 3.2 / Table 3-2): RESET#/CDP is active-low with a
// built-in ~25k pull-up; a low pulse >4us resets the chip; POR after release is
// ~5-14ms. CRITICAL: do NOT actively drive the pin HIGH -- as the hub exits reset,
// a driven-high level enables the CDP charging-port mode and disables low-power
// sleep, which disturbs the downstream port. Release to Hi-Z instead and let the
// internal pull-up bring it high (equivalent to the datasheet's recommended series
// Schottky-diode-to-MCU arrangement).
static void reset_usb_hub(void) {
#if ORB_BOARD_ID == ORB_BOARD_ID_CUSTOM_REV_0_1
    // Emplace once; subsequent calls (runtime recovery) reuse the already-configured pin.
    if (!s_hub_rst) s_hub_rst.emplace(orb::board::pin_usb_hub_rst);
    orb::hal::Clock clk;
    s_hub_rst->assert_low();  // drive RESET# low (>4us; we hold 10ms)
    // hal::Clock::delay_ms wraps busy_wait_ms — pure timer wait, safe pre-scheduler and on
    // core1. Never sleep_ms: that blocks via FreeRTOS before the scheduler is up (deadlock).
    clk.delay_ms(10);
    s_hub_rst->release();     // release to Hi-Z; external pull-up -> high, no CDP mode
    clk.delay_ms(50);         // wait out the hub POR (~5-14ms) before host init
#endif
}

// Bounded auto-reboot warm-reset recovery, gated on authentication. A controller
// frozen across a warm reset can mount but stay a "zombie" -- enumerated yet silent
// (no heartbeat, LED off). Recovery is probabilistic, so we watchdog-reboot and retry
// until it comes back live (lands a functional controller within a reboot or two).
//
// The retry runs ONLY before auth: a controller is required to establish auth (and to
// re-establish it after an Xbox-initiated reset), so a zombie -- or a controller lost
// part-way through auth -- should be recovered. Once authenticated the controller is
// optional (the user may unplug it freely, drum input comes over serial), so we disarm
// and never reboot again. If nothing ever mounts there's no controller to recover, so
// we don't reboot then either. The attempt count rides through our watchdog reboots
// (scratch[7]) but resets on a fresh power-on / physical reset, so we never loop forever.
#define RECOV_SCRATCH 7u  // scratch[4..6] are used by the SDK/bootrom watchdog path; [7] is free with pc=0 reboots
#define RECOV_MAGIC 0x5A5A0000u
#define RECOV_MAX 4u
#define RECOV_SILENT_MS 3000u  // a mounted-but-silent controller this long is a zombie
static uint32_t g_recov_count = 0;

static void recovery_reboot_task(void) {
    static bool disarmed = false;
    static uint32_t silent_since_ms = 0;  // 0 = not currently tracking a silence period
    if (disarmed) return;

    // Prefer the non-reboot runtime recovery: while core1 is actively pulsing the hub
    // RESET# to bring a lost controller back, hold off (and reset our debounce so the
    // reboot timer starts fresh once it stands down). Only fall back to a watchdog
    // reboot if that runtime recovery exhausts its attempts.
    if (g_runtime_recovery_engaged) { silent_since_ms = 0; return; }

    if (orb::service::adapter().state() >= adapter_state_t::STATE_RUNNING) {  // authenticated -> controller now optional
        disarmed = true;
        watchdog_hw->scratch[RECOV_SCRATCH] = 0;  // clear so the next reset starts fresh
        if (g_recov_count) LOG_WARN(CAT_RECOV, "RECOVERY: authenticated after %lu reboot(s)",
                                    (unsigned long)g_recov_count);
        return;
    }
    if (orb::service::adapter().alive()) { silent_since_ms = 0; return; }  // live -> healthy, nothing to do
    if (!orb::service::adapter().seen())  { silent_since_ms = 0; return; }  // none present -> nothing to recover

    // A controller mounted but isn't sending a heartbeat (zombie, or lost before auth).
    // Debounce a brief blip (re-enumeration) before acting.
    uint32_t now = board_millis();
    if (silent_since_ms == 0) silent_since_ms = now ? now : 1u;
    if ((uint32_t)(now - silent_since_ms) < RECOV_SILENT_MS) return;

    if (g_recov_count >= RECOV_MAX) {  // a zombie that won't thaw across retries
        disarmed = true;
        watchdog_hw->scratch[RECOV_SCRATCH] = 0;
        LOG_ERR(CAT_RECOV, "RECOVERY: controller stayed silent after %u reboots; replug needed", RECOV_MAX);
        return;
    }
    // Probabilistic: reboot and try again -- a later attempt usually lands a live one.
    watchdog_hw->scratch[RECOV_SCRATCH] = RECOV_MAGIC | (g_recov_count + 1u);
    LOG_WARN(CAT_RECOV, "RECOVERY: controller silent pre-auth -> watchdog reboot %lu/%u",
             (unsigned long)(g_recov_count + 1u), RECOV_MAX);
    dlog_drain();  // flush the log before we go
    watchdog_reboot(0, 0, 0);
    while (1) tight_loop_contents();
}

static void init() {
    // 120 MHz (no overclock): with upstream Pico-PIO-USB (post-0.7.2 bus-turnaround
    // / handshake timing fixes), the controller enumerates reliably through the
    // CH334R repeater at the stock 120 MHz the PIO-USB library is designed for.
    set_sys_clock_khz(120000, true);

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

        // Recover our auto-reboot attempt count: only trust the scratch register if
        // *we* triggered this reboot via watchdog. A power-on / physical reset clears
        // it to 0 so the user always gets a fresh recovery budget.
        if (watchdog_caused_reboot() &&
            (watchdog_hw->scratch[RECOV_SCRATCH] & 0xFFFF0000u) == RECOV_MAGIC) {
            g_recov_count = watchdog_hw->scratch[RECOV_SCRATCH] & 0xFFFFu;
        } else {
            g_recov_count = 0;
        }
        watchdog_hw->scratch[RECOV_SCRATCH] = 0;
        LOG_INFO(CAT_RECOV, "RECOVERY: attempt count = %lu", (unsigned long)g_recov_count);
    }

    // The deferred log's USB-stick mirror (usb_log_write sink) was attached by
    // orb_log_init() above: core0 pushes drained bytes into the ring and core1
    // (which owns the USB host stack) writes them out to LOG.TXT on the drive.

    xbox_fifo_init();
    LOG_INFO(CAT_SYS, "finished initializing xbox fifo...");

    // LED: push-pull output, start low (auth not yet established).
    s_led.emplace(orb::board::pin_led, /*initial=*/false);

    // Reset the hub before bringing up the host so a warm/watchdog reset
    // re-enumerates the controller cleanly instead of staying wedged.
    reset_usb_hub();

    // The USB host stack now comes up inside usb_host_task on core1 once the
    // FreeRTOS scheduler launches that core (see main()).
    LOG_INFO(CAT_SYS, "starting usb device stack");
    tud_init(TUD_OPT_RHPORT);

    serial_midi_init();
    LOG_INFO(CAT_SYS, "finished initializing serial midi...");

    memset(out_packet.buffer, 0, sizeof(out_packet.buffer));

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
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Low-priority periodic background: controller-announce heartbeat (gated to STATE_INIT,
// fires ~every 2s internally), warm-reset zombie recovery, and draining the deferred
// log to UART/USB. Grouped because all three are coarse periodic chores.
void housekeeping_task(void *param) {
    (void)param;
    while (true) {
        announce_task();
        recovery_reboot_task();
        dlog_drain();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

int main() {
    init();
    app_start_tasks();      // creates + pins usb_host_task (core1) and the core0 tasks
    vTaskStartScheduler();  // launches core1; never returns
    for (;;) {
    }
}
