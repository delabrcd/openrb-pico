#pragma once

#include "orb_c_api.h"  // ORB_C_BEGIN/END (impl is C++ now; callers are C)
#include "xbox_one_protocol.h"

// Instrument identity. Unscoped enum (not enum class) on purpose: it is part of the C
// API its C callers (drums.c, guitar.c, midi.cpp, main.c) use bare -- DRUMS, GUITAR_ONE,
// etc. FIRST_INSTRUMENT == 0 so the values double as contiguous array indices; the
// N_INSTRUMENTS sentinel is the count.
typedef enum {
    FIRST_INSTRUMENT,
    GUITAR_ONE = FIRST_INSTRUMENT,
    GUITAR_TWO,
    DRUMS,
    N_INSTRUMENTS,
} instruments_e;

ORB_C_BEGIN

void notify_xbox_of_all_instruments(xbox_packet_t *scratch_space);
void notify_xbox_of_single_instrument(instruments_e instrument, xbox_packet_t *scratch_space);
void connect_instrument(instruments_e instrument, xbox_packet_t *scratch_space);
void disconnect_instrument(instruments_e instrument, xbox_packet_t *scratch_space);

ORB_C_END

