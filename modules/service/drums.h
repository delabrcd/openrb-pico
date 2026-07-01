#pragma once

// Drum engine, as a modern-C++ service (orb::service::DrumEngine). Declared here so
// orb::app::System can own an instance as a plain member; method bodies (and every helper
// table/function they use -- the note->lane map, the per-lane state machine) stay in
// drums.cpp, out-of-line, exactly where they lived before this class had a header (same
// "local mirror" pattern as InstrumentManager::kInstrumentCount in instrument_manager.h --
// see kNumOut below).

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "adapter_ctx.h"         // orb::service::AdapterState
#include "app_queues.h"          // midi_note_t
#include "hal/platform.hpp"      // orb::hal::Clock
#include "hihat_config.h"        // ORB_HIHAT_MODE
#include "instrument_manager.h"  // orb::service::InstrumentManager
#include "midi.h"                // orb::service::SerialMidi
#include "osal/queue.hpp"        // orb::osal::Queue
#include "packet_queue.h"        // orb::driver::DeviceTxFifo
#include "xbox_one_protocol.h"   // XboxPacket

namespace orb::service {

// Consumes parsed MIDI (NoteOn -> a pad/cymbal lane; ControlChange -> the gated hi-hat
// pedal state machine) from two sources -- the USB-host note queue (produced on core1 by
// drums_read_midi_host(), modules/driver/drums_midi_seam.cpp) and the core0 serial-MIDI
// parser -- and publishes an Xbox drum input packet, deduped per lane, with TRIGGER_HOLD_MS
// auto-clear and an ADAPTER_OUT_INTERVAL emit rate. No dependency on the vendored USB stack:
// the TinyUSB MIDI host callback seam, the connected-device address, and the FIFO drain all
// live in the driver seam TU; this class only reacts to
// on_midi_connected()/on_midi_disconnected().
//
// Threading: input_pkt_ is written on core0 ONLY (tick(): lane bits + init_packet + the
// fifo write). on_midi_connected/on_midi_disconnected run on core1 but only post a hot-plug
// event via instruments_.post_connect/post_disconnect -- they never touch input_pkt_.
class DrumEngine {
   public:
    DrumEngine(orb::service::AdapterState& adapter,
               orb::osal::Queue<midi_note_t, 32>& midi_notes,
               orb::service::SerialMidi& serial_midi,
               orb::driver::DeviceTxFifo<XboxPacket, 16>& txfifo,
               orb::service::InstrumentManager& instruments);

    // Mirror of the anonymous-namespace Output::NUM_OUT lane count in drums.cpp (kick + 4
    // pads + 3 cymbals). Needed here only to size midi_output_states_ below; the Output
    // enum itself -- and the note->lane table / helpers that use it -- stay file-local to
    // drums.cpp (private nested types aren't visible to that file's anonymous-namespace
    // helpers, so this mirrors the count rather than the type, exactly like
    // InstrumentManager::kInstrumentCount in instrument_manager.h/.cpp). Cross-checked by a
    // static_assert in drums.cpp.
    static constexpr std::size_t kNumOut = 8;

    // --- core0: drum_task body -----------------------------------------------------------
    void tick();

    // --- core1: TinyUSB host MIDI connect/disconnect (called from the driver seam) --------
    void on_midi_connected() { instruments_.post_connect(DRUMS); }
    void on_midi_disconnected() { instruments_.post_disconnect(DRUMS); }

    // --- core1: push one parsed USB-host MIDI message into the note queue for tick() (core0)
    // to consume. Called from the driver seam (drums_read_midi_host) via the SeamAnchor, so the
    // producer stays inside DI instead of reaching a global midi_note_send(). Non-blocking.
    void push_host_note(const midi_note_t& note) { midi_notes_.send(note); }

   private:
    struct output_state_t {
        orb::hal::Clock::time_point triggered_at;
        bool triggered;
    };

    void note_on(std::uint8_t note, std::uint8_t velocity);
#if ORB_HIHAT_MODE >= 1
    void control_change(std::uint8_t controller, std::uint8_t value);
#endif

    orb::service::AdapterState& adapter_;
    orb::osal::Queue<midi_note_t, 32>& midi_notes_;
    orb::service::SerialMidi& serial_midi_;
    orb::driver::DeviceTxFifo<XboxPacket, 16>& txfifo_;
    orb::service::InstrumentManager& instruments_;

    XboxPacket input_pkt_;
    std::array<output_state_t, kNumOut> midi_output_states_{};
    bool changed_ = false;
#if ORB_HIHAT_MODE >= 2
    // CC-keyed hi-hat openness, tracked on core0 from the pedal-position CC (see
    // docs/features/hihat-mode.md). false = closed (pedal down); init closed so a
    // resting/unknown pedal maps to the default yellow-cymbal lane.
    bool hh_open_ = false;
#endif
};

}  // namespace orb::service

// Plain C++ free function (every consumer is a C++ TU); thin forwarder into
// orb::service::DrumEngine::tick(), defined in the app-layer bridge (modules/app/system.cpp).

// Drives the drum input packet from the MIDI sources. Called every tick from the
// core0 drum_input_task (see app/main.cpp).
void drum_task();
