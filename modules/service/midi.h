#pragma once

#include <cstdint>
#include <utility>  // std::to_underlying

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

// Plain C++ free functions (every consumer is a C++ TU); defined in midi.cpp.
void serial_midi_init();
int serial_midi_read(std::uint8_t* buf);
