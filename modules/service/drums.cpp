/*
 * Drum engine, as a modern-C++ service (orb::service::DrumEngine). drum_task /
 * drums_read_midi_host are plain C++ free functions its C++ callers (main.cpp) use; the
 * TinyUSB host stack calls the tuh_midi_*_cb seam by C symbol. Same service-rewrite
 * pattern as adapter_ctx.cpp / instrument_manager.cpp: a C++ object owns the input packet
 * + the per-output trigger/aging state, thin shims forward into the single instance.
 *
 * The engine consumes parsed MIDI (NoteOn -> a pad/cymbal lane; ControlChange -> the
 * gated hi-hat pedal state machine) from two sources -- the USB-host note queue
 * (produced on core1 by drums_read_midi_host) and the core0 serial-MIDI parser -- and
 * publishes an Xbox drum input packet, deduped per lane, with TRIGGER_HOLD_MS auto-clear
 * and an ADAPTER_OUT_INTERVAL emit rate. Behaviour is preserved byte-for-byte vs the
 * previous drums.c; the MIDI_MAP switch is replaced by a constexpr note->output table
 * built from the SAME inc/midi_map.h X-macro (so the mapping is identical by construction).
 *
 * Hi-hat: everything the alternate (CC-keyed) mode adds is gated on ORB_HIHAT_MODE
 * (inc/hihat_config.h, see docs/features/hihat-mode.md). The DEFAULT build (mode 0)
 * compiles all of it out and is byte-identical to the pure note-relay firmware.
 *
 * KNOWN cross-core hazard (pre-existing, intentionally NOT fixed here): input_pkt_ is
 * written from BOTH cores -- core0 (drum_task: lane bits + init_packet) and core1
 * (tuh_midi mount/umount -> connect/disconnect_instrument, which writes the packet via
 * build_packet/init_packet). There is no lock or single-word-publish discipline on that
 * 64-byte packet, so a connect/disconnect concurrent with a core0 emit can tear. This
 * matches the previous behaviour and is out of scope for this refactor.
 */
#include <pico.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "adapter.h"
#include "adapter_ctx.h"
#include "bsp/board_api.h"
#include "drums.h"
#include "hihat_config.h"
#include "midi.h"
#include "orb_debug.h"
#include "orb_log.h"
#include "usb_midi_host.h"

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
constexpr std::size_t idx(Output o) { return static_cast<std::size_t>(o); }

constexpr std::size_t kNumOut = idx(Output::NUM_OUT);

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
Output __not_in_flash_func(get_output_for_note)(std::uint8_t note) {
    return note < kNoteTable.size() ? kNoteTable[note] : Output::NO_OUT;
}

// Set/clear one lane bit in the drum input packet. Kept in RAM (__not_in_flash_func).
void __not_in_flash_func(update_drum_state_with_midi_input)(Output out, std::uint8_t state,
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

inline midi_type_e get_type_from_status(std::uint8_t status) {
    if ((status < 0x80) || (status == static_cast<std::uint8_t>(midi_type_e::Undefined_F4)) ||
        (status == static_cast<std::uint8_t>(midi_type_e::Undefined_F5)) ||
        (status == static_cast<std::uint8_t>(midi_type_e::Undefined_FD)))
        return midi_type_e::InvalidType;  // Data bytes and undefined.

    if (status < 0xf0)
        // Channel message, remove channel nibble.
        return static_cast<midi_type_e>(status & 0xf0);

    return static_cast<midi_type_e>(status);
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
// constant-initialised (constinit -> no global constructor), reproducing the old static
// designated-initializer exactly: player DRUMS, CMD_INPUT frame, unknown = 0x01. The
// omitted members (the wla_header button bitfields + the trailing host-side bookkeeping)
// are intentionally value-initialised to 0, exactly as the original C aggregate left them
// -- silence the field-by-field warning for this one deliberate partial initializer.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
constexpr xbox_packet_t kInitialDrumPacket = {
    .wla_header =
        {
            .frame =
                {
                    .command = static_cast<std::uint8_t>(frame_command_e::CMD_INPUT),
                    .device_id = static_cast<std::uint8_t>(frame_type_e::TYPE_COMMAND),
                    .type = static_cast<std::uint8_t>(frame_type_e::TYPE_COMMAND),
                    .sequence = 0,
                    .length = sizeof(xb_one_drum_input_pkt_t) - sizeof(frame_t),
                },
            .playerId = static_cast<std::uint8_t>(instruments_e::DRUMS),
            .unknown = 0x01,
        },
};
#pragma GCC diagnostic pop

class DrumEngine {
   public:
    // --- core0: drum_task body --------------------------------------------------------
    void __not_in_flash_func(tick)() {
        if (orb::service::adapter().state() != adapter_state_t::STATE_RUNNING) return;

        static std::uint8_t pending_msg[48];
        static midi_type_e type;
        static std::uint32_t current_time;

        midi_note_t n;
        while (midi_note_recv(&n)) {
            type = get_type_from_status(n.data[0]);
            if (type == midi_type_e::NoteOn) note_on(n.data[1], n.data[2]);
#if ORB_HIHAT_MODE >= 1
            else if (type == midi_type_e::ControlChange)
                control_change(n.data[1], n.data[2]);
#endif
        }

        while (serial_midi_read(pending_msg)) {
            type = get_type_from_status(pending_msg[0]);
            if (type == midi_type_e::NoteOn) note_on(pending_msg[1], pending_msg[2]);
#if ORB_HIHAT_MODE >= 1
            else if (type == midi_type_e::ControlChange)
                control_change(pending_msg[1], pending_msg[2]);
#endif
        }

        current_time = board_millis();
        for (Output out : kAllOutputs) {
            output_state_t& st = midi_output_states_[idx(out)];
            if (!st.triggered) continue;

            std::uint32_t time_since_trigger = current_time - st.triggered_at;
            if (time_since_trigger > trigger_hold_ms) {
                LOG_DBG(CAT_DRUM, "NOTE OFF: %d", static_cast<int>(out));
                update_drum_state_with_midi_input(out, 0, &input_pkt_.drum_input);
                st.triggered = false;
                changed_ = true;
            }
        }

        if (changed_ && current_time - input_pkt_.triggered_time > adapter_out_interval) {
            init_packet(&input_pkt_, current_time, sizeof(xb_one_drum_input_pkt_t));
            xbox_fifo_write(&input_pkt_);
            changed_ = false;
        }
    }

    // --- core1: drain the USB-host MIDI FIFO ------------------------------------------
    // Same core as on_midi_mount (which sets midi_dev_addr_). Drains regardless of adapter
    // state so the FIFO can't overflow; hands each complete message to core0 via the queue.
    void __not_in_flash_func(read_midi_host)() {
        std::uint8_t cable_num;
        std::uint8_t msg[48];
        while (tuh_midi_stream_read(midi_dev_addr_, &cable_num, msg, sizeof(msg)) != 0) {
            midi_note_t note{{msg[0], msg[1], msg[2]}};
            midi_note_send(&note);
        }
    }

    // --- core1: TinyUSB host MIDI mount/umount ----------------------------------------
    void on_midi_mount(std::uint8_t dev_addr, std::uint8_t in_ep, std::uint8_t out_ep,
                       std::uint8_t num_cables_rx, std::uint16_t num_cables_tx) {
        LOG_INFO(CAT_DRUM,
                 "MIDI device address = %u, IN endpoint %u has %u cables, OUT endpoint %u has %u "
                 "cables",
                 dev_addr, in_ep & 0xf, num_cables_rx, out_ep & 0xf, num_cables_tx);

        if (midi_dev_addr_ == 0) {
            // then no MIDI device is currently connected
            midi_dev_addr_ = dev_addr;
            connect_instrument(DRUMS, &input_pkt_);
        } else {
            LOG_WARN(CAT_DRUM,
                     "A different USB MIDI Device is already connected. Only one device at a time "
                     "is supported in this program; device is disabled");
        }
    }

    void on_midi_umount(std::uint8_t dev_addr, std::uint8_t instance) {
        if (dev_addr == midi_dev_addr_) {
            midi_dev_addr_ = 0;
            LOG_INFO(CAT_DRUM, "MIDI device address = %d, instance = %d is unmounted", dev_addr,
                     instance);
            disconnect_instrument(DRUMS, &input_pkt_);
        } else {
            LOG_INFO(CAT_DRUM, "Unused MIDI device address = %d, instance = %d is unmounted",
                     dev_addr, instance);
        }
    }

   private:
    struct output_state_t {
        std::uint32_t triggered_at;
        bool triggered;
    };

    void note_on(std::uint8_t note, std::uint8_t velocity) {
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

        LOG_DBG(CAT_DRUM, "NOTE ON: %d %d", static_cast<int>(out), velocity);

        midi_output_states_[idx(out)].triggered = true;
        midi_output_states_[idx(out)].triggered_at = board_millis();
    }

#if ORB_HIHAT_MODE >= 1
    // Consumes a ControlChange. At mode 1 this only logs the CC for discovery (so the
    // owner can watch the UART and learn which CC# their kit sends for the hi-hat pedal
    // and its polarity). At mode >= 2 it also drives the pedal-openness state machine.
    void control_change(std::uint8_t controller, std::uint8_t value) {
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

    xbox_packet_t input_pkt_ = kInitialDrumPacket;
    std::uint8_t midi_dev_addr_ = 0;
    std::array<output_state_t, kNumOut> midi_output_states_{};
    bool changed_ = false;
#if ORB_HIHAT_MODE >= 2
    // CC-keyed hi-hat openness, tracked on core0 from the pedal-position CC (see
    // docs/features/hihat-mode.md). false = closed (pedal down); init closed so a
    // resting/unknown pedal maps to the default yellow-cymbal lane.
    bool hh_open_ = false;
#endif
};

constinit DrumEngine g_engine;

}  // namespace
}  // namespace orb::service

// --- boundary ----------------------------------------------------------------------------
// drum_task / drums_read_midi_host are plain C++ free functions declared in drums.h;
// tuh_midi_mount_cb / tuh_midi_umount_cb are the TinyUSB host vendor seam (C-linkage via
// usb_midi_host.h). They stay thin shims that forward into the single DrumEngine, keeping
// the same __not_in_flash_func placement the originals had (drum_task +
// drums_read_midi_host in RAM; the mount/umount cbs not).

void __not_in_flash_func(drum_task)() { orb::service::g_engine.tick(); }

void __not_in_flash_func(drums_read_midi_host)(void) { orb::service::g_engine.read_midi_host(); }

void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep, uint8_t num_cables_rx,
                       uint16_t num_cables_tx) {
    orb::service::g_engine.on_midi_mount(dev_addr, in_ep, out_ep, num_cables_rx, num_cables_tx);
}

// Invoked when device with hid interface is un-mounted
void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance) {
    orb::service::g_engine.on_midi_umount(dev_addr, instance);
}
