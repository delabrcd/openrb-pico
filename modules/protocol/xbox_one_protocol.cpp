/*
 * GIP (Xbox One) packet construction / parsing for the emulated wireless legacy adapter,
 * as modern C++. The public functions are plain C++ free functions (every consumer is a
 * C++ TU); the wire structs (frame_t, xbox_packet_t, the input packets) stay the unchanged
 * packed/standard-layout aggregates declared in the header.
 *
 * CRITICAL: every emitted/parsed byte is identical to the original C -- this builds the
 * drum/guitar input packets and parses controller input.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "bsp/board_api.h"
#include "orb_debug.h"
#include "orb_enum.hpp"
#include "xbox_one_protocol.h"

namespace {

// GIP frame sequence counter -- post-increment wraps at 256, byte-for-byte the original
// `static uint8_t sequence` + `sequence++`. constinit -> BSS, no global ctor.
constinit std::uint8_t g_sequence = 0;

// Guitar HID input report as it arrives on the wire (PDP/legacy guitar). Internal to this
// TU; standard-layout packed so a byte-wise copy reconstructs it from the raw report.
struct hid_input_report_t {
    std::uint8_t cmd_id : 8;  // 00

    // 00
    std::uint8_t green : 1;   // Green button (bit 0)
    std::uint8_t red : 1;     // Red button (bit 1)
    std::uint8_t yellow : 1;  // Yellow button (bit 2)
    std::uint8_t blue : 1;    // Blue button (bit 3)
    std::uint8_t orange : 1;  // Orange button (bit 4)
    std::uint8_t unknown : 1;
    std::uint8_t select : 1;  // Select button (bit 7)
    std::uint8_t start : 1;   // Start button (bit 8)

    // 00
    std::uint8_t dpad_maybe : 8;

    // 00
    // strum up -       00
    // strum down -     04
    // strum center -   08
    std::uint8_t strum_bits : 8;

    std::uint8_t whammy_bits : 8;
    std::uint8_t dunno1 : 8;
    std::uint8_t tilt_bits : 8;
} __attribute__((packed));

// 4-bit colored-button state packed from the guitar HID fret bits -- identical shifts to
// the original MAKE_COLORED_STATE macro.
constexpr std::uint8_t make_colored_state(const hid_input_report_t &r) {
    return (r.blue << 2) | (r.green << 0) | (r.red << 1) | (r.yellow << 3);
}

}  // namespace

std::uint8_t get_sequence() { return g_sequence++; }

std::uint8_t xboxp_get_size(const xbox_packet_t *packet) {
    if (!packet) return 0;
    return packet->length;
}

void init_packet(xbox_packet_t *pkt, std::uint32_t time, std::uint8_t length) {
    pkt->frame.sequence = get_sequence();
    pkt->triggered_time = time;
    pkt->handled = 0;
    pkt->length = length;
}

void fill_guitar_input_from_hid_report(const std::uint8_t *report, xbox_packet_t *wla_output,
                                       std::uint8_t player_id) {
    init_packet(wla_output, board_millis(), sizeof(xb_one_guitar_input_pkt_t));

    wla_output->frame.command = frame_command_e::CMD_INPUT;
    wla_output->frame.device_id = 0;
    wla_output->frame.type = frame_type_e::TYPE_COMMAND;
    wla_output->frame.length = sizeof(xb_one_guitar_input_pkt_t) - sizeof(frame_t);
    wla_output->wla_header.playerId = player_id;

    xb_one_guitar_input_pkt_t *guitar_pkt = &wla_output->guitar_input;

    // Reconstruct the typed HID report from the raw bytes via a byte-wise copy -- avoids the
    // strict-aliasing UB of the original `(const hid_input_report_t *)report` cast.
    hid_input_report_t hid_report{};
    std::memcpy(&hid_report, report, sizeof(hid_report));

    guitar_pkt->wla_header.coloredButtonState1 = make_colored_state(hid_report);
    guitar_pkt->coloredButtonState2 = guitar_pkt->wla_header.coloredButtonState1;
    guitar_pkt->orangeButton = hid_report.orange;
    guitar_pkt->startButton = hid_report.start;
    guitar_pkt->selectButton = hid_report.select | (hid_report.tilt_bits > 128);
    guitar_pkt->whammy = hid_report.whammy_bits;

    if (hid_report.strum_bits == 0x00) {
        guitar_pkt->dpadState2 = (1 << 0);
    } else if (hid_report.strum_bits & 0x04) {
        guitar_pkt->dpadState2 = (1 << 1);
    } else if (hid_report.strum_bits & 0x08) {
        guitar_pkt->dpadState2 = 0;
    }
    guitar_pkt->wla_header.dpadState1 = guitar_pkt->dpadState2;
}

void fill_drum_input_from_controller(const xbox_packet_t *controller_input,
                                     xbox_packet_t *wla_output, std::uint8_t player_id) {
    std::ranges::fill(std::span{wla_output->buffer}, std::uint8_t{0});

    wla_output->handled = 0;
    wla_output->triggered_time = 0;

    wla_output->length = sizeof(xb_one_drum_input_pkt_t);
    wla_output->frame.command = controller_input->frame.command;
    wla_output->frame.device_id = controller_input->frame.device_id;
    wla_output->frame.type = static_cast<frame_type_e>(controller_input->frame.device_id);
    wla_output->frame.sequence = get_sequence();
    wla_output->frame.length = sizeof(xb_one_drum_input_pkt_t) - sizeof(frame_t);

    wla_output->wla_header.playerId = player_id;

    wla_output->wla_header.dpadState1 = controller_input->controller_input.buttons.dpadState;
    wla_output->drum_input.dpadState2 = controller_input->controller_input.buttons.dpadState;

    wla_output->wla_header.coloredButtonState1 =
            controller_input->controller_input.buttons.coloredButtonState;
    wla_output->drum_input.coloredButtonState2 =
            controller_input->controller_input.buttons.coloredButtonState;

    wla_output->wla_header.select = controller_input->controller_input.buttons.select;
    wla_output->drum_input.select = controller_input->controller_input.buttons.select;

    wla_output->wla_header.start = controller_input->controller_input.buttons.start;
    wla_output->drum_input.start = controller_input->controller_input.buttons.start;
}

#if OPENRB_DEBUG_ENABLED
// magic_enum only reflects enumerators whose value falls in [enum_range::min, max]. The
// frame_command_e values are command BYTES spanning CMD_ACKNOWLEDGE (0x01) ..
// CMD_AUDIO_SAMPLES (0x60); pin the per-enum range to exactly that span so every command
// resolves. (0x60 happens to sit inside magic_enum's default [-128,127] window too, but
// pinning the range keeps the generated reflection table small and the intent explicit.)
template <>
struct magic_enum::customize::enum_range<frame_command_e> {
    static constexpr int min = 0x01;  // CMD_ACKNOWLEDGE
    static constexpr int max = 0x60;  // CMD_AUDIO_SAMPLES
};

// Replaces the hand-rolled switch with magic_enum, preserving the two cases where the old
// table diverged from the enumerator names so trace output stays byte-identical:
//   - CMD_ACKNOWLEDGE was abbreviated "CMD_ACK" (not "CMD_ACKNOWLEDGE")
//   - unknown / out-of-range commands rendered "Unknown CMD" (magic_enum yields "")
// magic_enum's names are null-terminated, so .data() is a valid C string for the %s seam.
// Takes the raw command byte (int) rather than frame_command_e on purpose: it is a
// debug-only trace helper, and keeping the int parameter leaves its codegen byte-identical
// to the pre-typing baseline. Callers pass std::to_underlying(frame.command) at the boundary.
const char *get_command_name(int cmd) {
    if (cmd == static_cast<int>(frame_command_e::CMD_ACKNOWLEDGE)) return "CMD_ACK";
    const std::string_view name = orb::enum_name(static_cast<frame_command_e>(cmd));
    return name.empty() ? "Unknown CMD" : name.data();
}
#endif
