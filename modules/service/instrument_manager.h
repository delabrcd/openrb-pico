#pragma once

#include <cstdint>

#include "xbox_one_protocol.h"

// Instrument identity. The enumerators are used bare throughout (DRUMS, GUITAR_ONE, etc.)
// so they are imported to global scope below. FIRST_INSTRUMENT == 0 so the values double
// as contiguous array indices; the N_INSTRUMENTS sentinel is the count.
enum class instruments_e : std::uint8_t {
    FIRST_INSTRUMENT,
    GUITAR_ONE = FIRST_INSTRUMENT,
    GUITAR_TWO,
    DRUMS,
    N_INSTRUMENTS,
};

// The enumerators are used bare throughout the instrument API (DRUMS, GUITAR_ONE, ...),
// so import them to global scope; call sites keep their spelling. Conversions to an
// index / player-id byte / printf arg use std::to_underlying at the boundary.
using enum instruments_e;

// Plain C++ free functions (every consumer is a C++ TU); defined in instrument_manager.cpp.
//
// Re-announce the currently-connected instruments to the console. These only READ the
// connection state and build into the CALLER's scratch, so they stay callable directly
// from the core0 device stack (announce / CMD_ANNOUNCE handling).
void notify_xbox_of_all_instruments(xbox_packet_t *scratch_space);
void notify_xbox_of_single_instrument(instruments_e instrument, xbox_packet_t *scratch_space);

// Driver-facing hot-plug API. connect/disconnect_instrument POST an event to the
// instrument owner task and return immediately -- they NEVER take a lock or block, so they
// are safe to call from the core1 USB-host mount/umount callbacks (the previous
// synchronous path took a FreeRTOS mutex on core1 and could wedge the PIO-USB host). The
// single owner task (instrument_task, core0) is the sole mutator of the connection state
// and the only place the add/drop packet is built + queued to the device.
void connect_instrument(instruments_e instrument);
void disconnect_instrument(instruments_e instrument);

// Owner-task plumbing. instrument_manager_init() creates the event queue (call once before
// the scheduler starts). instrument_manager_service() blocks for one event and applies it;
// the core0 instrument_task loops on it.
void instrument_manager_init();
void instrument_manager_service();

