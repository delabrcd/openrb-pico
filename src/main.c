#include <bsp/board_api.h>
#include <device/usbd.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/uart.h>
#include <host/usbh.h>
#include <pico/multicore.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adapter.h"
#include "dlog.h"
#include "drums.h"
#include "hardware/dma.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "identifiers.h"
#include "instrument_manager.h"
#include "midi.h"
#include "orb_bsp.h"
#include "orb_debug.h"
#include "packet_queue.h"
#include "pio_usb_configuration.h"
#include "xbox_controller_driver.h"
#include "xbox_device_driver.h"

#define HOST_CONTROLLER_ID 1
#define FIRST_XBOX_CONTROLLER_IDX 0

volatile adapter_state_t adapter_state = STATE_NONE;

static volatile uint8_t xbox_controller_idx = UINT8_MAX;
static volatile uint8_t xbox_controller_addr = UINT8_MAX;

static xbox_packet_t out_packet;

// tinyusb 0.18's enumeration uses blocking osal_task_delay() (= sleep_ms) on the
// host core. sleep_ms hangs on the core running Pico-PIO-USB (it waits on an
// alarm event that core never receives), which stalls enumeration right after
// attach. Override the weak delay with a timer-based busy-wait that works on
// either core and keeps the PIO SOF interrupt running.
void __not_in_flash_func(tusb_time_delay_ms_api)(uint32_t ms) {
    // Lock-free busy-wait on the raw timer: time_us_64()/busy_wait()/sleep_ms()
    // take a spin lock / wait on an alarm that hangs on the core running
    // Pico-PIO-USB, so they cannot be used on core1. timerawl is lock-free.
    uint32_t start = timer_hw->timerawl;
    uint32_t us = ms * 1000u;
    while ((uint32_t)(timer_hw->timerawl - start) < us) {
        tight_loop_contents();
    }
}

static inline void set_auth_led(bool val) { gpio_put(PIN_LED, val); }

static inline void set_usb_host(bool on) {
#if ORB_BOARD_ID == ORB_BOARD_ID_FEATHER
    static bool configured = false;
    if (!configured) {
        gpio_init(PIN_5V_EN);
        gpio_set_dir(PIN_5V_EN, GPIO_OUT);
        configured = true;
    }
    gpio_put(PIN_5V_EN, on);
#else
    (void)on;
#endif
}

void xboxd_on_reset_cb() {
    set_auth_led(false);

    // TODO CDD - look into a better way of reinitializing the USB Host stack than a hard reset
    if (adapter_state != STATE_INIT && adapter_state != STATE_NONE) watchdog_reboot(0, 0, 10);
}

static inline bool xboxh_send(const xbox_packet_t *buffer) {
    return xboxh_send_report(xbox_controller_addr, xbox_controller_idx, buffer, buffer->length);
}

void xboxh_mount_cb(uint8_t dev_addr, uint8_t instance) {
    OPENRB_DEBUG("Controller %d Connected\r\n", instance);
    if (xbox_controller_idx == UINT8_MAX) {
        xbox_controller_idx = instance;
        xbox_controller_addr = dev_addr;
    }
}

void xboxh_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    OPENRB_DEBUG("Controller %d Disconnected\r\n", instance);
    if (instance == xbox_controller_idx) {
        xbox_controller_idx = UINT8_MAX;
        xbox_controller_addr = UINT8_MAX;
    }
}

void handle_controller_packet_running(const xbox_packet_t *data) {
    switch (data->frame.command) {
        case CMD_GUIDE_BTN:
            xbox_fifo_write(data);
            break;

        case CMD_INPUT:
            fill_drum_input_from_controller(data, &out_packet, DRUMS);
            xbox_fifo_write(&out_packet);
            break;
        default:
            break;
    }
}

void xboxh_packet_received_cb(uint8_t idx, const xbox_packet_t *data, const uint8_t ndata) {
    if (idx != xbox_controller_idx) return;
    if (ndata < sizeof(frame_t)) return;
    OPENRB_DEBUG("IN FROM CONTROLLER: %s\r\n", get_command_name(data->frame.command));
    switch (adapter_state) {
        case STATE_AUTHENTICATING:
            xbox_fifo_write(data);
            break;
        // case STATE_POWER_OFF:
        //     break;
        case STATE_RUNNING:
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
    OPENRB_DEBUG("Sent Controller %d bytes (%s)\r\n", ndata, get_command_name(data->frame.command));
}

static void handle_auth(const xbox_packet_t *packet) {
    if (packet->frame.command == CMD_AUTHENTICATE && packet->frame.length == 2 &&
        packet->buffer[3] == 2 && packet->buffer[4] == 1 && packet->buffer[5] == 0) {
        set_auth_led(true);

        OPENRB_DEBUG("AUTHENTICATED!\r\n");
        adapter_state = STATE_RUNNING;

        notify_xbox_of_all_instruments(&out_packet);
    }

    OPENRB_DEBUG("Sending controller %d bytes\r\n", packet->length);
    xboxh_send(packet);
    return;
}

static void handle_identify(const xbox_packet_t *packet) {
    static uint8_t identify_sequence = 0;
    switch (packet->frame.command) {
        case CMD_IDENTIFY:
        case CMD_ACKNOWLEDGE:
            if (identify_sequence >= identifiers_get_n()) {
                OPENRB_DEBUG("Starting identify sequence over\r\n");
                identify_sequence = 0;
            }
            identifiers_get(identify_sequence, &out_packet);
            xbox_fifo_write(&out_packet);
            identify_sequence++;
            break;
        case CMD_AUTHENTICATE:
            OPENRB_DEBUG("Moving to Authenticate\r\n");
            adapter_state = STATE_AUTHENTICATING;
            return handle_auth(packet);
            break;
        default:
            break;
    }
    return;
}

static void handle_init(const xbox_packet_t *packet) {
    switch (packet->frame.command) {
        case CMD_IDENTIFY:
            OPENRB_DEBUG("Moving to Identify\r\n");
            adapter_state = STATE_IDENTIFYING;
            return handle_identify(packet);
        default:
            break;
    }
}

static void handle_running(const xbox_packet_t *packet) {
    switch (packet->frame.command) {
        case CMD_POWER_MODE:
            if (packet->power.data.data == POWER_OFF) {
                adapter_state = STATE_POWER_OFF;
                set_auth_led(false);
                set_usb_host(false);
            }
            break;
        case CMD_ACKNOWLEDGE:
            xboxh_send(packet);
            break;
        case CMD_LIST_CONNECTED_INSTRUMENTS:
            notify_xbox_of_all_instruments(&out_packet);
            break;
        case CMD_LIST_INSTRUMENT:
            notify_xbox_of_single_instrument(packet->buffer[4], &out_packet);
            break;
        default:
            break;
    }
    return;
}

static void handle_xboxd_packet(const xbox_packet_t *packet) {
    switch (adapter_state) {
        case STATE_NONE:
            return;
        case STATE_INIT:
            return handle_init(packet);
        case STATE_IDENTIFYING:
            return handle_identify(packet);
        case STATE_AUTHENTICATING:
            return handle_auth(packet);
        case STATE_RUNNING:
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
    if (adapter_state != STATE_INIT) return;

    static unsigned long last_announce_time = 0;
    if ((board_millis() - last_announce_time) > ANNOUNCE_INTERVAL_MS) {
        if (xbox_controller_idx < UINT8_MAX) {
            OPENRB_DEBUG("ANNOUNCING\r\n");
            identifiers_get_announce(&out_packet);
            xbox_fifo_write(&out_packet);
            last_announce_time = board_millis();
        }
    }
}

static void configure_host() {
    OPENRB_DEBUG("configuring usb host stack\r\n");

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIN_USB_HOST_DP;

    // find an unused channel
    pio_cfg.tx_ch = dma_claim_unused_channel(true);
    dma_channel_unclaim(pio_cfg.tx_ch);
    tuh_configure(HOST_CONTROLLER_ID, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(HOST_CONTROLLER_ID);
    OPENRB_DEBUG("finished configuring usb host\r\n");

    set_usb_host(true);
}

void core1_main(void);

// core1's first launch after a chip reset can be spuriously reset back into the
// bootrom (PC=0x184) ~25-50ms in: an early-launch FIFO-handshake race in
// multicore_launch_core1() if core0 launches before core1 has settled into the
// bootrom wait-for-vector loop. The pico-sdk 1.5.1 -> 2.2.0 boot-timing change
// exposed it. The Pico-PIO-USB examples avoid it by sleeping ~10ms before the
// reset+launch (and again at the top of core1_main). See PORTING.md.
static void launch_core1_robust(void) {
    sleep_ms(10);
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
}

void core1_main() {
    // Settle before bringing up PIO-USB (matches the Pico-PIO-USB examples'
    // sleep_ms(10)); lock-free timer wait to avoid any alarm-pool dependency.
    uint32_t t = timer_hw->timerawl;
    while ((uint32_t)(timer_hw->timerawl - t) < 10000u) tight_loop_contents();
    configure_host();
    while (true) {
        tuh_task();
    }
}

static void init() {
    // 240 MHz (not 120): the bit-banged Pico-PIO-USB host needs the finer sub-bit
    // timing resolution to drive a clean packet through the CH334R hub's repeater.
    // At 120 MHz the controller behind the hub fails its first SETUP intermittently
    // (no handshake) and never enumerates. See PORTING.md root cause #2.
    set_sys_clock_khz(240000, true);

    dlog_init();

#if OPENRB_DEBUG_ENABLED
    stdio_uart_init_full(DBG_UART_ID, 115200, DBG_UART_TX_PIN, DBG_UART_RX_PIN);
    OPENRB_DEBUG("openrb debug console initialized...\r\n");
#endif

    xbox_fifo_init();
    OPENRB_DEBUG("finished initializing xbox fifo...\r\n");

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, true);

    OPENRB_DEBUG("starting usb host stack\r\n");
    launch_core1_robust();

    OPENRB_DEBUG("starting usb device stack\r\n");
    tud_init(TUD_OPT_RHPORT);

    serial_midi_init();
    OPENRB_DEBUG("finished initializing serial midi...\r\n");

    memset(out_packet.buffer, 0, sizeof(out_packet.buffer));

    adapter_state = STATE_INIT;
    OPENRB_DEBUG("finished init, starting main process...\r\n");
}

int main() {
    init();
    while (true) {
        tud_task();
        announce_task();
        xboxd_send_task();
        drum_task();
        dlog_drain();
    }
}
