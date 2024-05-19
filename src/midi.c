#include "midi.h"

#include <stdint.h>
#include <string.h>

#include "bsp/board_api.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "instrument_manager.h"
#include "orb_debug.h"
#include "pins_rp2040_usbh.h"

static int count = 0;
static uint8_t note_on_message[3] = {NoteOn, 0, 0};
static int alarm_number = 0;

static volatile bool drums_connected = false;

// 15 minutes
static uint32_t serial_timeout_ms = 900000;

static midi_type_e get_type_from_status(uint8_t status) {
    if ((status < 0x80) || (status == Undefined_F4) || (status == Undefined_F5) ||
        (status == Undefined_FD))
        return InvalidType;  // Data bytes and undefined.

    if (status < 0xf0)
        // Channel message, remove channel nibble.
        return status & 0xf0;

    return status;
}

void on_disconnect_timeout_cb() {
    if (drums_connected) {
        disconnect_instrument(DRUMS);
        drums_connected = false;
    }
}
void setup_disconnect_timer() {
    alarm_number = hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback(alarm_number, on_disconnect_timeout_cb);
}

void reset_disconnect_timer() {
    hardware_alarm_cancel(alarm_number);  // Cancel any existing timer
    hardware_alarm_set_target(alarm_number, make_timeout_time_ms(serial_timeout_ms));
}

void serial_midi_init() {
    gpio_set_function(PIN_SERIAL1_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_SERIAL1_RX, GPIO_FUNC_UART);

    OPENRB_DEBUG("uart baud: %d", uart_init(uart0, 31250));  // MIDI baud rate

    setup_disconnect_timer();
}

int serial_midi_read(uint8_t* buf) {
    while (uart_is_readable(uart0)) {
        bool status_byte = false;
        uint8_t data = uart_getc(uart0);
        midi_type_e type = get_type_from_status(data);
        switch (type) {
            case NoteOn:
                status_byte = true;
                note_on_message[0] = data;
                count = 1;
                break;
            case InvalidType:
                // data
                if (count) {
                    note_on_message[count] = data;
                    count++;
                }
                break;
            case ActiveSensing:
                serial_timeout_ms = 1000;
                // fallthrough
            default:
                status_byte = true;
                count = 0;
                break;
        }

        if (status_byte) {
            if (!drums_connected) {
                connect_instrument(DRUMS);
                drums_connected = true;
            }
            reset_disconnect_timer();
        }

        if (count >= 3) {
            OPENRB_DEBUG("Found Note On\r\n");
            memcpy(buf, note_on_message, 3);
            count = 1;
            return 3;
        }
    }

    return 0;
}