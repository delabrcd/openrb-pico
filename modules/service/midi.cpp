#include "midi.h"

#include <cstring>
#include <utility>  // std::to_underlying

#include "core/section.hpp"
#include "hal/platform.hpp"
#include "hihat_config.h"
#include "orb_bsp.h"
#include "orb_debug.h"

// Timeout durations for the drum-disconnect timer (in milliseconds).
// NOTE: FIFTEEN_MINUTES was a misnomer in the original code -- the value 90000 ms is
// 90 seconds, not 15 minutes. The value is preserved exactly to keep behavior identical.
inline constexpr uint32_t kOneSecondMs = 1000u;
inline constexpr uint32_t kDisconnectTimeoutMs = 90000u;  // 90 s (not 15 min -- see note above)

// Forward declaration: the real (extern "C", ORB_FAST) definition sits at the bottom of this
// file, but SerialMidi::setup_disconnect_timer() (below) needs the symbol to pass to
// orb::osal::Timer::create().
extern "C" void on_disconnect_timeout_cb(TimerHandle_t xTimer);

namespace orb::service {

void SerialMidi::init() {
    // hal::Uart constructor calls uart_init + gpio_set_function; stores the handle.
    // Baud 31250 = standard MIDI rate. Actual rate may differ slightly; the original
    // code logged uart_init's return value but the HAL constructor doesn't surface it.
    uart_.emplace(MIDI_UART, orb::board::midi_uart_tx, orb::board::midi_uart_rx, 31250u);
    LOG_INFO(CAT_MIDI, "uart baud: %u", 31250u);

    setup_disconnect_timer();
}

void SerialMidi::setup_disconnect_timer() {
    disconnect_timer_.create("midi_disc", pdMS_TO_TICKS(kDisconnectTimeoutMs),
                             false /*one-shot*/, on_disconnect_timeout_cb, nullptr);
}

void ORB_FAST(SerialMidi::reset_disconnect_timer)() {
    // Set the new period and (re)start the one-shot timer. change_period() wraps
    // xTimerChangePeriod, which also starts/restarts the timer, giving the same
    // "cancel + re-arm" semantics as the old hardware_alarm code. Block time 0 --
    // don't block in this hot path.
    disconnect_timer_.change_period(
        pdMS_TO_TICKS(drums_sending_active_sense_ ? kOneSecondMs : kDisconnectTimeoutMs), 0);
}

int ORB_FAST(SerialMidi::read)(std::uint8_t* buf) {
    while (uart_->readable()) {
        bool status_byte = false;
        std::uint8_t data = static_cast<std::uint8_t>(uart_->read_byte());
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
                note_on_message_[0] = data;
                count_ = 1;
                break;
            case midi_type_e::InvalidType:
                // data
                if (count_) {
                    note_on_message_[count_] = data;
                    count_++;
                }
                break;
            case midi_type_e::ActiveSensing:
                drums_sending_active_sense_ = true;
                // fallthrough
            default:
                status_byte = true;
                count_ = 0;
                break;
        }

        if (status_byte) {
            if (!drums_connected_.load(std::memory_order_relaxed)) {
                instruments_.post_connect(DRUMS);
                drums_connected_.store(true, std::memory_order_relaxed);
            }
            reset_disconnect_timer();
        }

        if (count_ >= 3) {
            LOG_TRC(CAT_MIDI, "serial midi msg");
            memcpy(buf, note_on_message_, 3);
            count_ = 1;
            return 3;
        }
    }

    return 0;
}

void SerialMidi::on_disconnect_timeout() {
    if (drums_connected_.load(std::memory_order_relaxed)) {
        instruments_.post_disconnect(DRUMS);
        drums_connected_.store(false, std::memory_order_relaxed);
    }
}

}  // namespace orb::service

// --- boundary ----------------------------------------------------------------------------
// Timer-service-task callback (kernel-called, C-linkage symbol). Stays a free extern "C"
// function and stays in RAM (__not_in_flash_func) -- it just delegates to the bridge
// forwarder (system.cpp), which reaches the single SerialMidi instance owned by
// orb::app::System. The timer id is left null at create(), same as before.
extern "C" void ORB_FAST(on_disconnect_timeout_cb)(TimerHandle_t xTimer) {
    (void)xTimer;
    serial_midi_on_disconnect_timeout();
}
