#pragma once

// Plain C++ free functions (every consumer is a C++ TU); thin forwarders into
// orb::service::DrumEngine, defined in drums.cpp.

// Drives the drum input packet from the MIDI sources. Called every tick from the
// core0 drum_input_task (see app/main.cpp).
void drum_task();

// Runs on core1 (owns the USB host stack): drains the USB-host MIDI FIFO and pushes
// each parsed message onto the MIDI note queue for drum_task (core0) to consume.
void drums_read_midi_host(void);

