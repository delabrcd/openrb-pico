/*
 * Drum engine, as a modern-C++ service (orb::service::DrumEngine). The class is declared in
 * drums.h so orb::app::System can own an instance as a plain member; drum_task is a plain C++
 * free function its C++ callers (main.cpp) use via the app-layer bridge (system.cpp). No
 * dependency on the vendored USB stack: the TinyUSB MIDI host callback seam, the
 * connected-device address, and the USB-host MIDI FIFO drain (drums_read_midi_host) all live
 * in modules/driver/drums_midi_seam.cpp -- this engine only reacts to
 * on_midi_connected()/on_midi_disconnected() (drums.h). Same service-rewrite
 * pattern as adapter_ctx.cpp / instrument_manager.cpp: a C++ object owns the input packet + the
 * per-output trigger/aging state; the private helper tables/functions below stay file-local
 * (anonymous namespace) exactly as before -- DrumEngine's out-of-line method bodies, being
 * defined in this same TU, can still see them.
 *
 * The engine consumes parsed MIDI (NoteOn -> a pad/cymbal lane; ControlChange -> the
 * gated hi-hat pedal state machine) from two sources -- the USB-host note queue
 * (produced on core1 by drums_read_midi_host, driver/drums_midi_seam.cpp) and the core0
 * serial-MIDI parser -- and publishes an Xbox drum input packet, deduped per lane, with
 * TRIGGER_HOLD_MS auto-clear and an ADAPTER_OUT_INTERVAL emit rate. Behaviour is preserved
 * byte-for-byte vs the prior drums implementation; the MIDI_MAP switch is replaced by a
 * constexpr note->output table built from the SAME inc/midi_map.h X-macro (so the mapping is
 * identical by construction).
 *
 * Hi-hat: everything the alternate (CC-keyed) mode adds is gated on ORB_HIHAT_MODE
 * (inc/hihat_config.h, see docs/features/hihat-mode.md). The DEFAULT build (mode 0)
 * compiles all of it out and is byte-identical to the pure note-relay firmware.
 *
 * Threading: input_pkt_ is written on core0 ONLY (tick: lane bits + init_packet +
 * the fifo write). on_midi_connected/on_midi_disconnected run on core1 but only post a hot-plug
 * event via instruments_.post_connect/post_disconnect -- they no longer build into input_pkt_
 * (the instrument owner task owns its own scratch), so the previous cross-core tearing hazard
 * on this 64-byte packet is gone.
 */
#include "core/section.hpp"
#include "hal/platform.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>  // std::to_underlying

#include "adapter.h"
#include "adapter_ctx.h"
#include "drums.h"
#include "hihat_config.h"
#include "midi.h"
#include "orb_debug.h"
#include "orb_log.h"

// instrument_manager.h (which pulls in xbox_one_protocol.h), packet_queue.h and
// xbox_one_protocol.h are all C++ headers now (init_packet, xbox_fifo_write, the
// connect/disconnect API are plain C++ free functions in C++ TUs), so include them
// normally.
#include "instrument_manager.h"
#include "packet_queue.h"
#include "xbox_one_protocol.h"

#include "app_queues.h"  // midi_note queue

namespace orb::service {
namespace {

// Rock Band drum output lane. Internal to the engine (not part of any C ABI), so a scoped
// enum -- but the enumerators keep their OUT_* names so the shared inc/midi_map.h X-macro
// table (which writes bare OUT_KICK etc.) resolves to Output::OUT_KICK by token paste.
enum class Output : std::uint8_t {
    OUT_KICK,
    OUT_PAD_RED,
    OUT_PAD_YELLOW,
    OUT_PAD_BLUE,
    OUT_PAD_GREEN,
    OUT_CYM_YELLOW,
    OUT_CYM_BLUE,
    OUT_CYM_GREEN,
    NUM_OUT,
    NO_OUT,
};

// Enum value -> contiguous array index.
constexpr std::size_t idx(Output o) { return std::to_underlying(o); }

constexpr std::size_t kNumOut = idx(Output::NUM_OUT);

// DrumEngine::kNumOut (drums.h) mirrors this count to size midi_output_states_ without
// exposing this file-local Output enum through the header -- keep them in sync.
static_assert(kNumOut == DrumEngine::kNumOut, "DrumEngine::kNumOut must mirror Output::NUM_OUT");

// Every real lane, in order -- for the range-for aging sweep (static_cast<int> of each
// equals its array index, matching the original "%d" log).
constexpr std::array<Output, kNumOut> kAllOutputs{
    Output::OUT_KICK,      Output::OUT_PAD_RED,    Output::OUT_PAD_YELLOW, Output::OUT_PAD_BLUE,
    Output::OUT_PAD_GREEN, Output::OUT_CYM_YELLOW, Output::OUT_CYM_BLUE,   Output::OUT_CYM_GREEN};

// note (0..127) -> Output, built from the SAME inc/midi_map.h X-macro that drove the old
// switch (so the mapping is identical by construction). Unmapped notes -> NO_OUT.
constexpr std::array<Output, 128> make_note_table() {
    std::array<Output, 128> t{};
    for (Output& e : t) e = Output::NO_OUT;
#define MIDI_MAP(midi_note, rb_out) t[midi_note] = Output::rb_out;
#include "midi_map.h"
#undef MIDI_MAP
    return t;
}
constexpr std::array<Output, 128> kNoteTable = make_note_table();

// Pure note->lane lookup. Kept in RAM (__not_in_flash_func) as before; a 7-bit MIDI note
// indexes the table, anything out of range maps to NO_OUT (the old switch's default).
Output ORB_FAST(get_output_for_note)(std::uint8_t note) {
    return note < kNoteTable.size() ? kNoteTable[note] : Output::NO_OUT;
}

// Set/clear one lane bit in the drum input packet. Kept in RAM (__not_in_flash_func).
void ORB_FAST(update_drum_state_with_midi_input)(Output out, std::uint8_t state,
                                                            xb_one_drum_input_pkt_t* drum_input) {
    switch (out) {
        case Output::OUT_KICK:
            drum_input->kick = state;
            break;
        case Output::OUT_PAD_RED:
            drum_input->pad_red = state;
            break;
        case Output::OUT_PAD_BLUE:
            drum_input->pad_blue = state;
            break;
        case Output::OUT_PAD_GREEN:
            drum_input->pad_green = state;
            break;
        case Output::OUT_PAD_YELLOW:
            drum_input->pad_yellow = state;
            break;
        case Output::OUT_CYM_YELLOW:
            drum_input->cymbal_yellow = state;
            break;
        case Output::OUT_CYM_BLUE:
            drum_input->cymbal_blue = state;
            break;
        case Output::OUT_CYM_GREEN:
            drum_input->cymbal_green = state;
            break;
        default:
            break;
    }
}


#if ORB_HIHAT_MODE >= 2
// Catch a manual misconfig at build time: threshold +/- hysteresis must stay within
// 0..127, else the open or close transition can never be reached. Relax/remove when these
// move to the runtime configurator (which validates its inputs).
static_assert(ORB_HIHAT_HYST <= ORB_HIHAT_THRESHOLD && ORB_HIHAT_THRESHOLD + ORB_HIHAT_HYST <= 127,
              "hi-hat threshold +/- hysteresis must stay within 0..127");

// Membership test over the configured hi-hat strike-note set.
bool is_hihat_note(std::uint8_t note) {
    static constexpr std::array kHihatNotes = std::to_array<std::uint8_t>(ORB_HIHAT_NOTES);
    return std::ranges::find(kHihatNotes, note) != kHihatNotes.end();
}
#endif

// Initial drum input packet. Built as a constant expression so the engine instance is
// constant-initialised, reproducing the old static designated-initializer exactly: player
// DRUMS, CMD_INPUT frame, unknown = 0x01. The omitted members (the wla_header button
// bitfields + the trailing host-side bookkeeping) are intentionally value-initialised to 0,
// exactly as the original C aggregate left them -- silence the field-by-field warning for
// this one deliberate partial initializer.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
constexpr xbox_packet_t kInitialDrumPacket = {
    .wla_header =
        {
            .frame =
                {
                    .command = frame_command_e::CMD_INPUT,
                    .device_id = std::to_underlying(frame_type_e::TYPE_COMMAND),
                    .type = frame_type_e::TYPE_COMMAND,
                    .sequence = 0,
                    .length = sizeof(xb_one_drum_input_pkt_t) - sizeof(frame_t),
                },
            .playerId = std::to_underlying(instruments_e::DRUMS),
            .unknown = 0x01,
        },
};
#pragma GCC diagnostic pop

}  // namespace

DrumEngine::DrumEngine(orb::service::AdapterState& adapter,
                       orb::osal::Queue<midi_note_t, 32>& midi_notes,
                       orb::service::SerialMidi& serial_midi,
                       orb::driver::DeviceTxFifo<xbox_packet_t, 16>& txfifo,
                       orb::service::InstrumentManager& instruments)
    : adapter_(adapter),
      midi_notes_(midi_notes),
      serial_midi_(serial_midi),
      txfifo_(txfifo),
      instruments_(instruments),
      input_pkt_(kInitialDrumPacket) {}

// --- core0: drum_task body -----------------------------------------------------------
void ORB_FAST(DrumEngine::tick)() {
    if (adapter_.state() != adapter_state_t::STATE_RUNNING) return;

    static midi_type_e type;

    midi_note_t n;
    while (midi_notes_.recv(n)) {
        type = midi_type_from_status(n.data[0]);
        if (type == midi_type_e::NoteOn) note_on(n.data[1], n.data[2]);
#if ORB_HIHAT_MODE >= 1
        else if (type == midi_type_e::ControlChange)
            control_change(n.data[1], n.data[2]);
#endif
    }

    while (auto msg = serial_midi_.read()) {
        type = midi_type_from_status((*msg)[0]);
        if (type == midi_type_e::NoteOn) note_on((*msg)[1], (*msg)[2]);
#if ORB_HIHAT_MODE >= 1
        else if (type == midi_type_e::ControlChange)
            control_change((*msg)[1], (*msg)[2]);
#endif
    }

    const orb::hal::Clock::time_point now = orb::hal::Clock{}.now();
    for (Output out : kAllOutputs) {
        output_state_t& st = midi_output_states_[idx(out)];
        if (!st.triggered) continue;

        if (now - st.triggered_at > trigger_hold) {
            LOG_DBG(CAT_DRUM, "NOTE OFF: %d", std::to_underlying(out));
            update_drum_state_with_midi_input(out, 0, &input_pkt_.drum_input);
            st.triggered = false;
            changed_ = true;
        }
    }

    if (changed_ && now.time_since_epoch() - input_pkt_.triggered_time > adapter_out_interval) {
        init_packet(&input_pkt_, now, sizeof(xb_one_drum_input_pkt_t));
        txfifo_.write(input_pkt_);
        changed_ = false;
    }
}

void DrumEngine::note_on(std::uint8_t note, std::uint8_t velocity) {
    if (velocity <= velocity_thresh) return;

    Output out = get_output_for_note(note);
#if ORB_HIHAT_MODE >= 2
    // CC-keyed override: for a hi-hat strike, ignore the note's table mapping and
    // emit open/closed from the tracked pedal state (closed->yellow, open->blue).
    if (is_hihat_note(note)) out = hh_open_ ? Output::OUT_CYM_BLUE : Output::OUT_CYM_YELLOW;
#endif
    if (out == Output::NO_OUT) return;

    if (midi_output_states_[idx(out)].triggered) return;

    update_drum_state_with_midi_input(out, 1, &input_pkt_.drum_input);
    changed_ = true;

    LOG_DBG(CAT_DRUM, "NOTE ON: %d %d", std::to_underlying(out), velocity);

    midi_output_states_[idx(out)].triggered = true;
    midi_output_states_[idx(out)].triggered_at = orb::hal::Clock{}.now();
}

#if ORB_HIHAT_MODE >= 1
// Consumes a ControlChange. At mode 1 this only logs the CC for discovery (so the
// owner can watch the UART and learn which CC# their kit sends for the hi-hat pedal
// and its polarity). At mode >= 2 it also drives the pedal-openness state machine.
void DrumEngine::control_change(std::uint8_t controller, std::uint8_t value) {
#if ORB_HIHAT_MODE >= 2
    if (controller == ORB_HIHAT_CC) {
        // "raw closeness" rises with the configured-closed direction.
        std::uint8_t v = ORB_HIHAT_INVERT ? static_cast<std::uint8_t>(127 - value) : value;
        if (hh_open_) {
            if (v >= ORB_HIHAT_THRESHOLD + ORB_HIHAT_HYST) hh_open_ = false;  // -> closed
        } else {
            if (v <= ORB_HIHAT_THRESHOLD - ORB_HIHAT_HYST) hh_open_ = true;  // -> open
        }
    }
#endif
#if ORB_HIHAT_MODE == 1
    LOG_INFO(CAT_DRUM, "CC %u = %u", controller, value);  // discovery: visible by default
#else
    LOG_TRC(CAT_DRUM, "CC %u = %u", controller, value);  // mode 2: off at the default floor
#endif
}
#endif

}  // namespace orb::service
