#pragma once

#include "orb_c_api.h"  // ORB_C_BEGIN/END (impl is C++ now; callers are C)

ORB_C_BEGIN

// Drives the drum input packet from the MIDI sources. Called every tick from the
// core0 drum_input_task (see src/main.c). Stays a C-linkage entry point: it is
// invoked from C and implemented as an extern "C" shim into orb::service::DrumEngine.
void drum_task();

// Runs on core1 (owns the USB host stack): drains the USB-host MIDI FIFO and pushes
// each parsed message onto the MIDI note queue for drum_task (core0) to consume.
void drums_read_midi_host(void);

ORB_C_END

