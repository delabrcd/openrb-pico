#pragma once

#include <cstdint>

#include "xbox_one_protocol.h"

// Instrument identity. Unscoped enum (not enum class) on purpose: it is part of the C
// API its C callers (drums.c, guitar.c, midi.cpp, main.c) use bare -- DRUMS, GUITAR_ONE,
// etc. FIRST_INSTRUMENT == 0 so the values double as contiguous array indices; the
// N_INSTRUMENTS sentinel is the count.
enum class instruments_e : std::uint8_t {
    FIRST_INSTRUMENT,
    GUITAR_ONE = FIRST_INSTRUMENT,
    GUITAR_TWO,
    DRUMS,
    N_INSTRUMENTS,
};

// The enumerators are used bare throughout the instrument API (DRUMS, GUITAR_ONE, ...),
// so import them to global scope; call sites keep their spelling. Conversions to an
// index / player-id byte / printf arg still static_cast explicitly at the boundary.
using enum instruments_e;

// Plain C++ free functions (every consumer is a C++ TU); defined in instrument_manager.cpp.
void notify_xbox_of_all_instruments(xbox_packet_t *scratch_space);
void notify_xbox_of_single_instrument(instruments_e instrument, xbox_packet_t *scratch_space);
void connect_instrument(instruments_e instrument, xbox_packet_t *scratch_space);
void disconnect_instrument(instruments_e instrument, xbox_packet_t *scratch_space);

