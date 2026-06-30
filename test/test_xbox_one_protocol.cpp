/*
 * HOST unit tests for the portable protocol module (src/xbox_one_protocol.cpp).
 *
 * These link the REAL protocol TU (built for the host via test/CMakeLists.txt) against
 * no-op log stubs + a fake board_millis(), and assert the exact bytes the protocol emits.
 *
 * Golden vectors below were derived by hand from the packed wire structs in
 * inc/xbox_one_protocol.h and cross-checked against the implementation. The sizeof()
 * sanity checks guard the layout assumptions the golden offsets depend on; if a struct
 * grows/shifts, those fail first and pinpoint the golden vector that needs updating.
 *
 * Sequence note: get_sequence() is a process-global post-increment counter shared across
 * the whole binary, so absolute sequence bytes are not hardcoded. Each test that triggers
 * a get_sequence() captures the counter immediately before the call and asserts the
 * emitted sequence == captured + 1, which is deterministic regardless of test order.
 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "host_test_support.h"
#include "xbox_one_protocol.h"

using std::uint8_t;
using std::uint32_t;

namespace {

// Pretty per-byte comparison so a mismatch reports index + expected/actual hex.
void check_bytes(const uint8_t* actual, const uint8_t* expected, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        INFO("byte index ", i);
        CHECK(static_cast<int>(actual[i]) == static_cast<int>(expected[i]));
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Layout invariants the golden vectors rely on.
// ---------------------------------------------------------------------------------------
TEST_CASE("wire struct sizes match the documented layout") {
    CHECK(sizeof(frame_t) == 4);
    CHECK(sizeof(power_report_t) == 5);
    CHECK(sizeof(led_mode_command_t) == 7);
    CHECK(sizeof(xb_one_wireless_legacy_adapter_pkt_t) == 8);
    CHECK(sizeof(xb_one_drum_input_pkt_t) == 20);
    CHECK(sizeof(xb_one_guitar_input_pkt_t) == 20);
    // The leading anonymous union is exactly one max USB endpoint packet.
    CHECK(offsetof(xbox_packet_t, length) == XBOX_ONE_EP_MAXPKTSIZE);
}

// ---------------------------------------------------------------------------------------
// get_sequence / xboxp_get_size
// ---------------------------------------------------------------------------------------
TEST_CASE("get_sequence is a wrapping post-increment counter") {
    uint8_t a = get_sequence();
    uint8_t b = get_sequence();
    uint8_t c = get_sequence();
    CHECK(static_cast<uint8_t>(b - a) == 1);
    CHECK(static_cast<uint8_t>(c - b) == 1);

    // Wrap at 256: 256 increments return to the same value.
    uint8_t start = get_sequence();
    for (int i = 0; i < 255; ++i) get_sequence();
    CHECK(get_sequence() == start);
}

TEST_CASE("xboxp_get_size returns length, 0 on null") {
    xbox_packet_t pkt{};
    pkt.length = 0x2A;
    CHECK(xboxp_get_size(&pkt) == 0x2A);
    CHECK(xboxp_get_size(nullptr) == 0);
}

// ---------------------------------------------------------------------------------------
// init_packet
// ---------------------------------------------------------------------------------------
TEST_CASE("init_packet stamps length/time/handled and advances sequence") {
    xbox_packet_t pkt{};
    pkt.handled = 0xFF;  // must be cleared

    uint8_t seq_before = get_sequence();
    init_packet(&pkt, 0xDEADBEEF, 0x14);

    CHECK(pkt.length == 0x14);
    CHECK(pkt.triggered_time == 0xDEADBEEF);
    CHECK(pkt.handled == 0);
    CHECK(pkt.frame.sequence == static_cast<uint8_t>(seq_before + 1));
}

// ---------------------------------------------------------------------------------------
// make_power_report  (header static-inline builder)
// ---------------------------------------------------------------------------------------
TEST_CASE("make_power_report golden bytes") {
    // CMD_POWER_MODE=0x05, device_id=0,type=REQUEST=0x2 -> byte1=0x20, length=1.
    power_report_t pr = make_power_report(
            /*sequence=*/0x42, /*data=*/static_cast<uint8_t>(power_mode_e::POWER_OFF) /*0x04*/);

    const std::array<uint8_t, 5> golden = {0x05, 0x20, 0x42, 0x01, 0x04};
    check_bytes(pr.buffer, golden.data(), golden.size());

    // Field-level cross-check.
    CHECK(pr.data.frame.command == frame_command_e::CMD_POWER_MODE);
    CHECK(pr.data.frame.type == frame_type_e::TYPE_REQUEST);
    CHECK(pr.data.frame.device_id == 0);
    CHECK(pr.data.frame.sequence == 0x42);
    CHECK(pr.data.frame.length == 1);
    CHECK(pr.data.data == static_cast<uint8_t>(power_mode_e::POWER_OFF));
}

// ---------------------------------------------------------------------------------------
// make_led_mode_command  (header static-inline builder)
// ---------------------------------------------------------------------------------------
TEST_CASE("make_led_mode_command golden bytes") {
    // CMD_LED_MODE=0x0a, byte1=0x20, length=3, unknown=0, mode, brightness.
    led_mode_command_t led = make_led_mode_command(
            /*sequence=*/0x07, /*mode=*/led_mode_e::LED_ON /*0x01*/,
            /*brightness=*/0x14);

    const std::array<uint8_t, 7> golden = {0x0a, 0x20, 0x07, 0x03, 0x00, 0x01, 0x14};
    auto* bytes = reinterpret_cast<const uint8_t*>(&led);
    check_bytes(bytes, golden.data(), golden.size());

    CHECK(led.frame.command == frame_command_e::CMD_LED_MODE);
    CHECK(led.frame.type == frame_type_e::TYPE_REQUEST);
    CHECK(led.frame.length == 3);
    CHECK(led.unknown == 0);
    CHECK(led.mode == led_mode_e::LED_ON);
    CHECK(led.brightness == 0x14);
}

// ---------------------------------------------------------------------------------------
// fill_drum_input_from_controller
// ---------------------------------------------------------------------------------------
TEST_CASE("fill_drum_input_from_controller golden bytes") {
    // Build a known controller-input source packet by field name.
    xbox_packet_t in{};
    in.controller_input.frame.command = frame_command_e::CMD_INPUT;  // 0x20
    in.controller_input.frame.device_id = 0;
    in.controller_input.buttons.start = 1;
    in.controller_input.buttons.select = 1;
    in.controller_input.buttons.coloredButtonState = 0x05;  // 0101
    in.controller_input.buttons.dpadState = 0x0A;           // 1010

    xbox_packet_t out{};
    const uint8_t player_id = 0x03;

    uint8_t seq_before = get_sequence();
    fill_drum_input_from_controller(&in, &out, player_id);

    // Expected 20-byte drum wire packet (sizeof xb_one_drum_input_pkt_t):
    //  [0] command           = 0x20 (copied from controller)
    //  [1] device_id|type<<4 = 0x00 (both 0; note: type is set from device_id)
    //  [2] sequence          = seq_before + 1
    //  [3] frame.length      = 20 - 4 = 0x10
    //  [4] wla: start(b2)|select(b3)|colored1<<4 = 0x04|0x08|0x50 = 0x5C
    //  [5] wla: dpadState1(0xA) = 0x0A
    //  [6] playerId          = 0x03
    //  [7] unknown           = 0x00
    //  [8] drum: start(b2)|select(b3)|colored2<<4 = 0x5C
    //  [9] drum: dpadState2(0xA) = 0x0A
    //  [10..19] zeroed (no pad/cymbal hits)
    std::array<uint8_t, 20> golden = {0x20, 0x00, 0x00, 0x10, 0x5C, 0x0A, 0x03, 0x00,
                                      0x5C, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00};
    golden[2] = static_cast<uint8_t>(seq_before + 1);

    check_bytes(out.buffer, golden.data(), golden.size());

    // Bookkeeping field cross-checks.
    CHECK(out.length == sizeof(xb_one_drum_input_pkt_t));
    CHECK(out.triggered_time == 0);
    CHECK(out.handled == 0);
}

// ---------------------------------------------------------------------------------------
// fill_guitar_input_from_hid_report
// ---------------------------------------------------------------------------------------
TEST_CASE("fill_guitar_input_from_hid_report golden bytes") {
    // Pin the fake clock so triggered_time is assertable.
    // now_us() returns g_host_fake_us; triggered_time = now_us() / 1000.
    // Use 0x12345u ms -> 0x12345u * 1000 us = 74565000 us (fits in uint32_t).
    g_host_fake_us = 0x12345u * 1000u;

    // Raw 7-byte PDP/legacy guitar HID report.
    //  byte0 cmd_id       = 0x00
    //  byte1 fret/btn bits= 0x95 -> green=1 red=0 yellow=1 blue=0 orange=1 sel=0 start=1
    //  byte2 dpad_maybe   = 0x00
    //  byte3 strum_bits   = 0x04 -> dpadState2 = 1<<1 = 2
    //  byte4 whammy_bits  = 0x7F
    //  byte5 dunno1       = 0x00
    //  byte6 tilt_bits    = 0xFF -> >128, forces selectButton high
    const std::array<uint8_t, 7> report = {0x00, 0x95, 0x00, 0x04, 0x7F, 0x00, 0xFF};

    xbox_packet_t out{};
    const uint8_t player_id = 0x02;

    uint8_t seq_before = get_sequence();
    fill_guitar_input_from_hid_report(report.data(), &out, player_id);

    // make_colored_state = blue<<2|green|red<<1|yellow<<3 = 0|1|0|8 = 0x09.
    // Expected 20-byte guitar wire packet (sizeof xb_one_guitar_input_pkt_t):
    //  [0] command            = 0x20 (CMD_INPUT)
    //  [1] device_id|type<<4  = 0x00
    //  [2] sequence           = seq_before + 1
    //  [3] frame.length       = 20 - 4 = 0x10
    //  [4] wla: colored1<<4   = 0x90  (wla start/select NOT written by guitar path)
    //  [5] wla: dpadState1(2) = 0x02
    //  [6] playerId           = 0x02
    //  [7] unknown            = 0x00
    //  [8] startButton(b2)|selectButton(b3)|colored2<<4 = 0x04|0x08|0x90 = 0x9C
    //  [9] dpadState2(2)|orangeButton(b4) = 0x02|0x10 = 0x12
    //  [10] (anon)            = 0x00
    //  [11] whammy            = 0x7F
    //  [12..19] zeroed
    std::array<uint8_t, 20> golden = {0x20, 0x00, 0x00, 0x10, 0x90, 0x02, 0x02, 0x00,
                                      0x9C, 0x12, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00};
    golden[2] = static_cast<uint8_t>(seq_before + 1);

    check_bytes(out.buffer, golden.data(), golden.size());

    CHECK(out.length == sizeof(xb_one_guitar_input_pkt_t));
    CHECK(out.triggered_time == 0x12345u);  // g_host_fake_us / 1000
    CHECK(out.handled == 0);
}

// A second guitar case: strum-center clears the strum dpad bit, no tilt, select bit
// comes purely from the report's own select bit (set here), whammy mid-scale.
TEST_CASE("fill_guitar_input_from_hid_report strum-center / select-from-report") {
    g_host_fake_us = 0;

    //  byte1 = 0x42 -> red=1 (b1), select=1 (b6); green/yellow/blue/orange/start = 0
    //  byte3 strum_bits = 0x08 -> dpadState2 = 0 (center)
    //  byte6 tilt = 0x10 (< 128) -> no tilt-forced select
    const std::array<uint8_t, 7> report = {0x00, 0x42, 0x00, 0x08, 0x20, 0x00, 0x10};

    xbox_packet_t out{};
    fill_guitar_input_from_hid_report(report.data(), &out, /*player_id=*/0x01);

    // colored = blue<<2|green|red<<1|yellow<<3 = 0|0|0x2|0 = 0x02.
    CHECK(out.guitar_input.wla_header.coloredButtonState1 == 0x02);
    CHECK(out.guitar_input.coloredButtonState2 == 0x02);
    CHECK(out.guitar_input.dpadState2 == 0);            // strum center
    CHECK(out.guitar_input.wla_header.dpadState1 == 0);
    CHECK(out.guitar_input.selectButton == 1);          // from report select bit
    CHECK(out.guitar_input.startButton == 0);
    CHECK(out.guitar_input.orangeButton == 0);
    CHECK(out.guitar_input.whammy == 0x20);
    CHECK(out.guitar_input.wla_header.playerId == 0x01);
}
