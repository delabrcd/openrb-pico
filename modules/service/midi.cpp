#include "midi.h"

#include <array>
#include <chrono>
#include <optional>
#include <utility>  // std::to_underlying

#include "core/section.hpp"
#include "hal/platform.hpp"
#include "hihat_config.h"
#include "orb_bsp.h"
#include "orb_debug.h"

// Drum-disconnect timer intervals. NOTE: the original FIFTEEN_MINUTES name was a misnomer --
// 90000 ms is 90 s, preserved exactly.
inline constexpr std::chrono::seconds kOneSecond{1};
inline constexpr std::chrono::seconds kDisconnectTimeout{90};  // 90 s (not 15 min)

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
    disconnect_timer_.create<SerialMidi, &SerialMidi::on_disconnect_timeout>(
        "midi_disc", kDisconnectTimeout, false /*one-shot*/, *this);
}

void ORB_FAST(SerialMidi::reset_disconnect_timer)() {
    // Set the new period and (re)start the one-shot timer. change_period() wraps
    // xTimerChangePeriod, which also starts/restarts the timer, giving the same
    // "cancel + re-arm" semantics as the old hardware_alarm code. Block time 0 --
    // don't block in this hot path.
    disconnect_timer_.change_period(drums_sending_active_sense_ ? kOneSecond : kDisconnectTimeout);
}

std::optional<std::array<std::uint8_t, 3>> ORB_FAST(SerialMidi::read)() {
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
            std::array<std::uint8_t, 3> msg{note_on_message_[0], note_on_message_[1],
                                             note_on_message_[2]};
            count_ = 1;
            return msg;
        }
    }

    return std::nullopt;
}

void SerialMidi::on_disconnect_timeout() {
    if (drums_connected_.load(std::memory_order_relaxed)) {
        instruments_.post_disconnect(DRUMS);
        drums_connected_.store(false, std::memory_order_relaxed);
    }
}

}  // namespace orb::service
