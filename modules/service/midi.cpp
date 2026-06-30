#include "midi.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>

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
static std::uint8_t note_on_message[3] = {std::to_underlying(midi_type_e::NoteOn), 0, 0};
// orb::osal::Timer owns the StaticTimer_t control block (BSS, trivial ctor) and the handle,
// replacing the old raw TimerHandle_t + StaticTimer_t + xTimerCreateStatic plumbing.
static orb::osal::Timer s_disconnect_timer;

// Written and read on core0 (serial_midi_read, on_disconnect_timeout_cb timer task);
// atomic for consistency with the project's cross-task-state idiom.
static std::atomic<bool> drums_connected{false};
static bool drums_sending_active_sense = false;

// Timeout durations for the drum-disconnect timer (in milliseconds).
// NOTE: FIFTEEN_MINUTES was a misnomer in the original code -- the value 90000 ms is
// 90 seconds, not 15 minutes. The value is preserved exactly to keep behavior identical.
inline constexpr uint32_t kOneSecondMs = 1000u;
inline constexpr uint32_t kDisconnectTimeoutMs = 90000u;  // 90 s (not 15 min -- see note above)

static xbox_packet_t out_packet;

// Timer-service-task callback (kernel-called, C-linkage symbol). Stays a free
// extern "C" function and stays in RAM (__not_in_flash_func) — it finds its state via
// the file-static drums_connected / out_packet, exactly as before, so the timer id is
// left null at create().
extern "C" void ORB_FAST(on_disconnect_timeout_cb)(TimerHandle_t xTimer) {
    (void)xTimer;
    if (drums_connected.load(std::memory_order_relaxed)) {
        disconnect_instrument(DRUMS, &out_packet);
        drums_connected.store(false, std::memory_order_relaxed);
    }
}

static void setup_disconnect_timer() {
    s_disconnect_timer.create("midi_disc", pdMS_TO_TICKS(kDisconnectTimeoutMs),
                              false /*one-shot*/, on_disconnect_timeout_cb, nullptr);
}

static void ORB_FAST(reset_disconnect_timer)() {
    // Set the new period and (re)start the one-shot timer. change_period() wraps
    // xTimerChangePeriod, which also starts/restarts the timer, giving the same
    // "cancel + re-arm" semantics as the old hardware_alarm code. Block time 0 —
    // don't block in this hot path.
    s_disconnect_timer.change_period(
        pdMS_TO_TICKS(drums_sending_active_sense ? kOneSecondMs : kDisconnectTimeoutMs), 0);
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

int ORB_FAST(serial_midi_read)(std::uint8_t* buf) {
    while (s_midi_uart->readable()) {
        bool status_byte = false;
        std::uint8_t data = static_cast<std::uint8_t>(s_midi_uart->read_byte());
        midi_type_e type = midi_type_from_status(data);
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
            if (!drums_connected.load(std::memory_order_relaxed)) {
                connect_instrument(DRUMS, &out_packet);
                drums_connected.store(true, std::memory_order_relaxed);
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
