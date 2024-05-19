#include <bsp/board_api.h>
#include <device/usbd.h>
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
#include "drums.h"
#include "hardware/dma.h"
#include "identifiers.h"
#include "instrument_manager.h"
#include "midi.h"
#include "orb_debug.h"
#include "packet_queue.h"
#include "pins_rp2040_usbh.h"
#include "pio_usb_configuration.h"
#include "xbox_controller_driver.h"
#include "xbox_device_driver.h"

#define HOST_CONTROLLER_ID 1

#define FIRST_XBOX_CONTROLLER_IDX 0

volatile adapter_state_t adapter_state = STATE_NONE;

static volatile uint8_t xbox_controller_idx = UINT8_MAX;
static volatile uint8_t xbox_controller_addr = UINT8_MAX;

static xbox_packet_t out_packet;

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
        gpio_put(PIN_LED, true);
        OPENRB_DEBUG("AUTHENTICATED!\r\n");
        adapter_state = STATE_RUNNING;

        notify_xbox_of_all_instruments();
    }

    printf("Sending controller %d bytes\n", packet->length);
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
            // TODO CDD - go to bed
            break;
        case CMD_ACKNOWLEDGE:
            xboxh_send(packet);
            break;

        case CMD_LIST_CONNECTED_INSTRUMENTS:
            notify_xbox_of_all_instruments();
            break;
        case CMD_LIST_INSTRUMENT:
            notify_xbox_of_single_instrumenty(packet->buffer[4]);
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
    gpio_init(PIN_5V_EN);
    gpio_set_dir(PIN_5V_EN, GPIO_OUT);
    gpio_put(PIN_5V_EN, 1);

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIN_USB_HOST_DP;

    // find an unused channel
    pio_cfg.tx_ch = dma_claim_unused_channel(true);
    dma_channel_unclaim(pio_cfg.tx_ch);
    tuh_configure(HOST_CONTROLLER_ID, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(HOST_CONTROLLER_ID);
    OPENRB_DEBUG("finished configuring usb host\r\n");
}

void core1_main() {
    configure_host();
    while (true) {
        tuh_task();
    }
}

#define UART_ID uart1
#define UART_TX_PIN 24
#define UART_RX_PIN 25

static void init() {
    set_sys_clock_khz(120000, true);

    stdio_uart_init_full(UART_ID, 115200, UART_TX_PIN, UART_RX_PIN);
    OPENRB_DEBUG("openrb debug console initialized...\r\n");

    xbox_fifo_init();
    OPENRB_DEBUG("finished initializing xbox fifo...\r\n");

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, true);

    OPENRB_DEBUG("starting usb host stack\r\n");
    multicore_reset_core1();
    multicore_launch_core1(core1_main);

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
    }
}
