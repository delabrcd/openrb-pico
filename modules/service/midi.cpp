#include "midi.h"

#include <optional>
#include <stdint.h>
#include <string.h>

#include <utility>  // std::to_underlying

#include "core/section.hpp"
#include "hal/platform.hpp"
#include "hihat_config.h"
#include "orb_bsp.h"
#include "orb_debug.h"
#include "osal/timer.hpp"

// instrument_manager.h (and the xbox_one_protocol.h it pulls in) is a C++ header now;
// connect/disconnect_instrument are plain C++ free functions, so include it normally.
#include "instrument_manager.h"

// MIDI UART handle — emplaced once by serial_midi_init(), before the scheduler.
// std::optional so the constructor (which calls uart_init + gpio_set_function) runs only
// in serial_midi_init(), not at static-init time (modern-cpp.md hardware-object lifetime).
static std::optional<orb::hal::Uart> s_midi_uart;

static int count = 0;
static uint8_t note_on_message[3] = {std::to_underlying(midi_type_e::NoteOn), 0, 0};
// orb::osal::Timer owns the StaticTimer_t control block (BSS, trivial ctor) and the handle,
// replacing the old raw TimerHandle_t + StaticTimer_t + xTimerCreateStatic plumbing.
static orb::osal::Timer s_disconnect_timer;

static volatile bool drums_connected = false;
static bool drums_sending_active_sense = false;

#define ONE_SECOND 1000
#define FIFTEEN_MINUTES 90000

static xbox_packet_t out_packet;

static inline midi_type_e get_type_from_status(uint8_t status) {
    if ((status < 0x80) || (status == std::to_underlying(midi_type_e::Undefined_F4)) ||
        (status == std::to_underlying(midi_type_e::Undefined_F5)) ||
        (status == std::to_underlying(midi_type_e::Undefined_FD)))
        return midi_type_e::InvalidType;  // Data bytes and undefined.

    if (status < 0xf0)
        // Channel message, remove channel nibble.
        return static_cast<midi_type_e>(status & 0xf0);

    return static_cast<midi_type_e>(status);
}

// Timer-service-task callback (kernel-called, C-linkage symbol). Stays a free
// extern "C" function and stays in RAM (__not_in_flash_func) — it finds its state via
// the file-static drums_connected / out_packet, exactly as before, so the timer id is
// left null at create().
extern "C" void ORB_FAST(on_disconnect_timeout_cb)(TimerHandle_t xTimer) {
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

static void ORB_FAST(reset_disconnect_timer)() {
    // Set the new period and (re)start the one-shot timer. change_period() wraps
    // xTimerChangePeriod, which also starts/restarts the timer, giving the same
    // "cancel + re-arm" semantics as the old hardware_alarm code. Block time 0 —
    // don't block in this hot path.
    s_disconnect_timer.change_period(
        pdMS_TO_TICKS(drums_sending_active_sense ? ONE_SECOND : FIFTEEN_MINUTES), 0);
}

void serial_midi_init() {
    // hal::Uart constructor calls uart_init + gpio_set_function; stores the handle.
    // Baud 31250 = standard MIDI rate. Actual rate may differ slightly; the original
    // code logged uart_init's return value but the HAL constructor doesn't surface it.
    s_midi_uart.emplace(MIDI_UART, orb::board::midi_uart_tx, orb::board::midi_uart_rx,
                        31250u);
    LOG_INFO(CAT_MIDI, "uart baud: %u", 31250u);

    setup_disconnect_timer();
}

int ORB_FAST(serial_midi_read)(uint8_t* buf) {
    while (s_midi_uart->readable()) {
        bool status_byte = false;
        uint8_t data = static_cast<uint8_t>(s_midi_uart->read_byte());
        midi_type_e type = get_type_from_status(data);
        switch (type) {
#if ORB_HIHAT_MODE >= 1
            // CC-keyed hi-hat mode also needs ControlChange off the serial path.
            // Treat it exactly like NoteOn: valid status byte, collect 2 data
            // bytes, return the raw 3-byte message; a CC also counts as "drums
            // connected" (status_byte path below), so the disconnect timer behaves.
            case midi_type_e::ControlChange:
#endif
            case midi_type_e::NoteOn:
                status_byte = true;
                note_on_message[0] = data;
                count = 1;
                break;
            case midi_type_e::InvalidType:
                // data
                if (count) {
                    note_on_message[count] = data;
                    count++;
                }
                break;
            case midi_type_e::ActiveSensing:
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
            LOG_TRC(CAT_MIDI, "serial midi msg");
            memcpy(buf, note_on_message, 3);
            count = 1;
            return 3;
        }
    }

    return 0;
}
