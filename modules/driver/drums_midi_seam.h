#pragma once

// TinyUSB host MIDI seam for the drum service (orb::service::DrumEngine). Binds a single
// orb::core::SeamAnchor<DrumEngine> at init -- before tuh_init() runs on core1 -- and the
// extern "C" tuh_midi_*_cb callbacks (defined in drums_midi_seam.cpp) forward straight into
// the service with a direct concrete call. This is the tusb-owning half of the drums MIDI
// seam that used to live in modules/service/drums.cpp; DrumEngine itself (drums.h/.cpp)
// stays tusb-free -- see modules/core/seam_anchor.hpp for the pattern.

#include "drums.h"  // orb::service::DrumEngine

namespace orb::driver {

void bind_drums_midi(orb::service::DrumEngine& engine);

}  // namespace orb::driver

// Runs on core1 (owns the USB host stack): drains the USB-host MIDI FIFO and pushes each
// parsed message onto the MIDI note queue for drum_task (core0) to consume. Moved here from
// DrumEngine::read_midi_host so the tusb call (tuh_midi_stream_read) lives in driver/, not
// service/. Called every core1 loop iteration from main.cpp.
void drums_read_midi_host(void);
