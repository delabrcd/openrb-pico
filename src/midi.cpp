#include "midi.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "orb_bsp.h"
#include "orb_debug.h"
#include "software_timer.hpp"

// instrument_manager.h (and the xbox_one_protocol.h it pulls in) is a plain C header
// with no C-linkage seam of its own; this is the only C++ TU that includes it, so wrap
// it locally so connect/disconnect_instrument resolve to their C definitions.
extern "C" {
#include "instrument_manager.h"
}

static int count = 0;
static uint8_t note_on_message[3] = {NoteOn, 0, 0};
// SoftwareTimer owns the StaticTimer_t control block (BSS, trivial ctor) and the handle,
// replacing the old raw TimerHandle_t + StaticTimer_t + xTimerCreateStatic plumbing.
static SoftwareTimer s_disconnect_timer;

static volatile bool drums_connected = false;
static bool drums_sending_active_sense = false;

#define ONE_SECOND 1000
#define FIFTEEN_MINUTES 90000

static xbox_packet_t out_packet;

static inline midi_type_e get_type_from_status(uint8_t status) {
    if ((status < 0x80) || (status == Undefined_F4) || (status == Undefined_F5) ||
        (status == Undefined_FD))
        return InvalidType;  // Data bytes and undefined.

    if (status < 0xf0)
        // Channel message, remove channel nibble.
        return (midi_type_e)(status & 0xf0);

    return (midi_type_e)status;
}

// Timer-service-task callback (kernel-called, C-linkage symbol). Stays a free
// extern "C" function and stays in RAM (__not_in_flash_func) — it finds its state via
// the file-static drums_connected / out_packet, exactly as before, so the timer id is
// left null at create().
extern "C" void __not_in_flash_func(on_disconnect_timeout_cb)(TimerHandle_t xTimer) {
    (void)xTimer;
    if (drums_connected) {
        disconnect_instrument(DRUMS, &out_packet);
        drums_connected = false;
    }
}

static void setup_disconnect_timer() {
    s_disconnect_timer.create("midi_disc", pdMS_TO_TICKS(FIFTEEN_MINUTES),
                              false /*one-shot*/, on_disconnect_timeout_cb, nullptr);
}

static void __not_in_flash_func(reset_disconnect_timer)() {
    // Set the new period and (re)start the one-shot timer. change_period() wraps
    // xTimerChangePeriod, which also starts/restarts the timer, giving the same
    // "cancel + re-arm" semantics as the old hardware_alarm code. Block time 0 —
    // don't block in this hot path.
    s_disconnect_timer.change_period(
        pdMS_TO_TICKS(drums_sending_active_sense ? ONE_SECOND : FIFTEEN_MINUTES), 0);
}

void serial_midi_init() {
    gpio_set_function(MIDI_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(MIDI_UART_RX, GPIO_FUNC_UART);

    // NB: keep uart_init out of the OPENRB_DEBUG(...) argument — when debug is
    // disabled the macro expands to nothing and the UART would never initialize.
    uint actual_baud = uart_init(MIDI_UART, 31250);
    OPENRB_DEBUG("uart baud: %u\r\n", actual_baud);

    setup_disconnect_timer();
}

int __not_in_flash_func(serial_midi_read)(uint8_t* buf) {
    while (uart_is_readable(MIDI_UART)) {
        bool status_byte = false;
        uint8_t data = uart_getc(MIDI_UART);
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
                drums_sending_active_sense = true;
                // fallthrough
            default:
                status_byte = true;
                count = 0;
                break;
        }

        if (status_byte) {
            if (!drums_connected) {
                connect_instrument(DRUMS, &out_packet);
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
