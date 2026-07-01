#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>  // std::to_underlying

#include "hal/platform.hpp"       // orb::hal::Uart
#include "instrument_manager.h"   // orb::service::InstrumentManager
#include "osal/timer.hpp"         // orb::osal::Timer

enum class midi_type_e : std::uint8_t {
    InvalidType = 0x00,                      ///< For notifying errors
    NoteOff = 0x80,                          ///< Channel Message - Note Off
    NoteOn = 0x90,                           ///< Channel Message - Note On
    AfterTouchPoly = 0xA0,                   ///< Channel Message - Polyphonic AfterTouch
    ControlChange = 0xB0,                    ///< Channel Message - Control Change / Channel Mode
    ProgramChange = 0xC0,                    ///< Channel Message - Program Change
    AfterTouchChannel = 0xD0,               ///< Channel Message - Channel (monophonic) AfterTouch
    PitchBend = 0xE0,                        ///< Channel Message - Pitch Bend
    SystemExclusive = 0xF0,                  ///< System Exclusive
    SystemExclusiveStart = SystemExclusive,  ///< System Exclusive Start
    TimeCodeQuarterFrame = 0xF1,             ///< System Common - MIDI Time Code Quarter Frame
    SongPosition = 0xF2,                     ///< System Common - Song Position Pointer
    SongSelect = 0xF3,                       ///< System Common - Song Select
    Undefined_F4 = 0xF4,
    Undefined_F5 = 0xF5,
    TuneRequest = 0xF6,         ///< System Common - Tune Request
    SystemExclusiveEnd = 0xF7,  ///< System Exclusive End
    Clock = 0xF8,               ///< System Real Time - Timing Clock
    Undefined_F9 = 0xF9,
    Tick = Undefined_F9,  ///< System Real Time - Timing Tick (1 tick = 10 milliseconds)
    Start = 0xFA,         ///< System Real Time - Start
    Continue = 0xFB,      ///< System Real Time - Continue
    Stop = 0xFC,          ///< System Real Time - Stop
    Undefined_FD = 0xFD,
    ActiveSensing = 0xFE,  ///< System Real Time - Active Sensing
    SystemReset = 0xFF,    ///< System Real Time - System Reset
};

// Map a MIDI status byte to its type. Data bytes (< 0x80) and the three undefined
// system-common codes (0xF4, 0xF5, 0xFD) return InvalidType. Shared by midi.cpp and
// drums.cpp; constexpr so it can be used in constant expressions.
constexpr midi_type_e midi_type_from_status(std::uint8_t status) noexcept {
    if ((status < 0x80) || (status == std::to_underlying(midi_type_e::Undefined_F4)) ||
        (status == std::to_underlying(midi_type_e::Undefined_F5)) ||
        (status == std::to_underlying(midi_type_e::Undefined_FD)))
        return midi_type_e::InvalidType;  // Data bytes and undefined.
    if (status < 0xf0)
        return static_cast<midi_type_e>(status & 0xf0);  // Channel message, remove channel nibble.
    return static_cast<midi_type_e>(status);
}

namespace orb::service {

// Serial (UART) MIDI parser + the drum-disconnect timer, as a modern-C++ service
// (orb::service::SerialMidi). Same service-rewrite pattern as InstrumentManager/DrumEngine:
// a C++ object owns the state, thin free functions (declared below, defined in the
// app-layer bridge -- this file is board-dependent so it stays out of the composition-root
// TU, see modules/service/CMakeLists.txt) forward into the single instance owned by
// orb::app::System.
//
// uart_ is std::optional so the hal::Uart constructor (uart_init + gpio_set_function) runs
// only in init(), not at System-construction time relative ordering concerns -- matching the
// hardware-object lifetime rule the original file-static idiom followed.
class SerialMidi {
   public:
    explicit SerialMidi(orb::service::InstrumentManager& instruments)
        : instruments_(instruments) {}

    // Bring up the MIDI UART (baud 31250) and arm the disconnect timer. Call once, before
    // the scheduler starts (matches the old serial_midi_init timing).
    void init();

    // Parse bytes off the UART; returns a complete 3-byte message when one is ready,
    // std::nullopt otherwise (was: returned 3 / 0 and wrote through a uint8_t* buffer).
    std::optional<std::array<std::uint8_t, 3>> read();

    // Drum-disconnect timeout body; invoked by the osal::Timer member-pointer callback shim
    // (see setup_disconnect_timer / osal::Timer::create<T,&M>). No C-linkage symbol remains.
    void on_disconnect_timeout();

   private:
    void setup_disconnect_timer();
    void reset_disconnect_timer();

    orb::service::InstrumentManager& instruments_;

    std::optional<orb::hal::Uart> uart_;
    orb::osal::Timer disconnect_timer_;

    int count_ = 0;
    std::uint8_t note_on_message_[3] = {std::to_underlying(midi_type_e::NoteOn), 0, 0};
    // Written/read on core0 (read(), on_disconnect_timeout()); atomic for consistency with
    // the project's cross-task-state idiom (the timer-service task is a separate task).
    std::atomic<bool> drums_connected_{false};
    bool drums_sending_active_sense_ = false;
};

}  // namespace orb::service

// Plain C++ free function (every consumer is a C++ TU); thin forwarder into the single
// orb::service::SerialMidi instance owned by orb::app::System, defined in the app-layer
// composition-root bridge (modules/app/system.cpp).
void serial_midi_init();
