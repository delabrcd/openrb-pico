#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "core/lifetime.hpp"  // orb::mem::start_lifetime_as
#include "hal/platform.hpp"
#include "orb_debug.h"

/* Xbox One data taken from descriptors: max wire packet size (BUCKET A). C++-only
 * constexpr -- every consumer is C++, and it is used both in a static_assert and to size
 * the wire union's buffer. */
inline constexpr std::size_t XBOX_ONE_EP_MAXPKTSIZE = 64;

// WIRE/ABI enums: an explicit uint8_t underlying type pins their size to one byte (these
// values index wire tables / are command bytes on the USB wire). Because each enum is
// exactly one byte, the packed-struct fields that carry one of these enums are typed with
// the enum directly (frame_t.command / frame_t.type, led_mode_command_t.mode): the field
// occupies the same single byte, so the __attribute__((packed)) layout + every
// sizeof/offsetof static_assert below stay valid, while comparisons/switches/assignments at
// call sites are strongly typed and need no cast. Genuine raw-byte <-> enum conversions
// (e.g. extracting a wire byte into an enum) use static_cast / std::to_underlying explicitly.
enum class frame_command_e : uint8_t {
    CMD_ACKNOWLEDGE = 0x01,
    CMD_ANNOUNCE = 0x02,
    CMD_STATUS = 0x03,
    CMD_IDENTIFY = 0x04,
    CMD_POWER_MODE = 0x05,
    CMD_AUTHENTICATE = 0x06,
    CMD_GUIDE_BTN = 0x07,
    CMD_AUDIO_CONFIG = 0x08,
    CMD_RUMBLE = 0x09,
    CMD_LED_MODE = 0x0a,
    CMD_SERIAL_NUM = 0x1e,
    CMD_INPUT = 0x20,
    CMD_LIST_INSTRUMENT = 0x21,
    CMD_ADD_PLAYER = 0x22,
    CMD_DROP_PLAYER = 0x23,
    CMD_LIST_CONNECTED_INSTRUMENTS = 0x24,
    CMD_AUDIO_SAMPLES = 0x60,
};

enum class frame_type_e : uint8_t {
    TYPE_COMMAND = 0x00,
    TYPE_ACK = 0x01,
    TYPE_REQUEST = 0x02,
};

enum class power_mode_e : uint8_t {
    POWER_ON = 0x00,
    POWER_SLEEP = 0x01,
    POWER_OFF = 0x04,
};

enum class led_mode_e : uint8_t {
    LED_OFF = 0x00,
    LED_ON = 0x01,
    LED_BLINK_FAST = 0x02,
    LED_BLINK_MED = 0x03,
    LED_BLINK_SLOW = 0x04,
    LED_FADE_SLOW = 0x08,
    LED_FADE_FAST = 0x09,
};

typedef struct {
    frame_command_e command;
    uint8_t device_id : 4;
    frame_type_e type : 4;
    uint8_t sequence;
    uint8_t length;
} __attribute__((packed)) frame_t;

// Small standalone wire message (CMD_POWER_MODE): a frame header + one power-mode byte.
// Plain packed struct -- no union. Its raw bytes for the USB send are read through
// unsigned char (the char-aliasing exception, always well-defined); see make_power_report's
// callers / xboxh_send_report.
typedef struct {
    frame_t frame;
    uint8_t data;
} __attribute__((packed)) power_report_t;

typedef struct {
    frame_t frame;
    uint8_t unknown;
    led_mode_e mode;
    uint8_t brightness;
} __attribute__((packed)) led_mode_command_t;

typedef struct {
    frame_t frame;
    struct Buttons {
        uint32_t : 2;
        uint32_t start : 1;
        uint32_t select : 1;

        uint8_t coloredButtonState : 4;

        uint8_t dpadState : 4;

        uint32_t bumperLeft : 1;
        uint32_t bumperRight : 1;
        uint32_t stickLeft : 1;
        uint32_t stickRight : 1;

    } __attribute__((packed)) buttons;

    uint16_t triggerLeft;
    uint16_t triggerRight;
    int16_t stickLeftX;
    int16_t stickLeftY;
    int16_t stickRightX;
    int16_t stickRightY;

} __attribute__((packed)) xb_one_controller_input_pkt_t;

typedef struct {
    frame_t frame;

    uint8_t : 2;
    uint8_t start : 1;
    uint8_t select : 1;

    uint8_t coloredButtonState1 : 4;

    uint8_t dpadState1 : 4;

    uint8_t left_bumper : 1;
    uint8_t right_bumper : 1;
    uint8_t : 2;

    uint8_t playerId;
    uint8_t unknown;
} __attribute__((packed)) xb_one_wireless_legacy_adapter_pkt_t;

typedef struct {
    xb_one_wireless_legacy_adapter_pkt_t wla_header;

    uint8_t : 2;
    uint8_t start : 1;
    uint8_t select : 1;

    uint8_t coloredButtonState2 : 4;

    uint8_t dpadState2 : 4;

    uint8_t kick : 1;
    uint8_t doublekick : 1;
    uint8_t : 2;

    uint8_t : 3;
    uint8_t pad_yellow : 1;

    uint8_t : 3;
    uint8_t pad_red : 1;

    uint8_t : 3;
    uint8_t pad_green : 1;

    uint8_t : 3;
    uint8_t pad_blue : 1;

    uint8_t : 3;
    uint8_t cymbal_blue : 1;

    uint8_t : 3;
    uint8_t cymbal_yellow : 1;

    uint8_t : 7;
    uint8_t cymbal_green : 1;

    uint8_t : 8;
    uint8_t : 8;

    uint8_t unused[4];
} __attribute__((packed)) xb_one_drum_input_pkt_t;

typedef struct {
    xb_one_wireless_legacy_adapter_pkt_t wla_header;

    uint8_t : 2;
    uint8_t startButton : 1;
    uint8_t selectButton : 1;

    uint8_t coloredButtonState2 : 4;

    uint8_t dpadState2 : 4;

    uint8_t orangeButton : 1;
    uint8_t : 3;

    uint8_t : 8;

    uint8_t whammy;

    uint8_t unused[8];
} __attribute__((packed)) xb_one_guitar_input_pkt_t;

// The Xbox wire packet: a fixed 64-byte wire payload plus host-side bookkeeping. Replaces
// the former type-punning union with typed accessors -- each returns a reference to the wire
// bytes viewed as the requested packet struct via orb::mem::start_lifetime_as (well-defined,
// zero-cost; see modules/core/lifetime.hpp). No `union`, and no reinterpret_cast at any call
// site. Raw-byte access (data()/wire()) is fine directly: the storage IS an unsigned-char
// array, and unsigned char may alias any object.
//
// Trivially copyable AND trivially default-constructible on purpose: it is memcpy'd through
// the device tx fifo / OSAL queues, and the TinyUSB driver structs that embed it are cleared
// with tu_memclr. So the data members carry NO in-class initializers -- callers zero/fill
// explicitly (data()/wire() + memset, or `= {}`), exactly as the old aggregate required.
class XboxPacket {
    // The 64-byte wire payload, declared FIRST and alignas(4). Two guarantees fall out:
    //   * data() (== &wire_[0]) is at object offset 0, so a stray `&packet` and packet.data()
    //     coincide -- defends against the class of relay bug where the raw object address (not
    //     the wire buffer) was handed to the USB send.
    //   * the buffer is word-aligned, matching the CFG_TUSB_MEM_ALIGN on the epin_buf/epout_buf
    //     that embed this packet, so the USB DMA copy lands on an aligned address.
    alignas(4) std::array<std::uint8_t, XBOX_ONE_EP_MAXPKTSIZE> wire_;

   public:
    // --- typed views over the wire payload (all alias the same leading bytes) ------------
    frame_t &frame() { return view<frame_t>(); }
    const frame_t &frame() const { return view<frame_t>(); }
    xb_one_wireless_legacy_adapter_pkt_t &wla_header() {
        return view<xb_one_wireless_legacy_adapter_pkt_t>();
    }
    const xb_one_wireless_legacy_adapter_pkt_t &wla_header() const {
        return view<xb_one_wireless_legacy_adapter_pkt_t>();
    }
    power_report_t &power() { return view<power_report_t>(); }
    const power_report_t &power() const { return view<power_report_t>(); }
    xb_one_controller_input_pkt_t &controller_input() { return view<xb_one_controller_input_pkt_t>(); }
    const xb_one_controller_input_pkt_t &controller_input() const {
        return view<xb_one_controller_input_pkt_t>();
    }
    xb_one_drum_input_pkt_t &drum_input() { return view<xb_one_drum_input_pkt_t>(); }
    const xb_one_drum_input_pkt_t &drum_input() const { return view<xb_one_drum_input_pkt_t>(); }
    xb_one_guitar_input_pkt_t &guitar_input() { return view<xb_one_guitar_input_pkt_t>(); }
    const xb_one_guitar_input_pkt_t &guitar_input() const { return view<xb_one_guitar_input_pkt_t>(); }

    // --- raw wire access -----------------------------------------------------------------
    std::uint8_t *data() { return wire_.data(); }
    const std::uint8_t *data() const { return wire_.data(); }
    std::span<std::uint8_t, XBOX_ONE_EP_MAXPKTSIZE> wire() { return wire_; }
    std::span<const std::uint8_t, XBOX_ONE_EP_MAXPKTSIZE> wire() const { return wire_; }
    // The bytes actually put on the wire for this packet (bounded by `length`).
    std::span<std::uint8_t> sent() { return {wire_.data(), length}; }
    std::span<const std::uint8_t> sent() const { return {wire_.data(), length}; }
    static constexpr std::size_t capacity() { return XBOX_ONE_EP_MAXPKTSIZE; }

    // --- host-side bookkeeping (public POD; never serialized past `length`) ---------------
    std::uint8_t length;
    // Time-since-epoch of the monotonic Clock (a Clock::duration, not a time_point, so it
    // stays trivially-copyable). Set/compared by the drivers; not on the wire.
    orb::hal::Clock::duration triggered_time;
    std::uint8_t handled;

   private:
    template <typename T>
    T &view() {
        return *orb::mem::start_lifetime_as<T>(wire_.data());
    }
    template <typename T>
    const T &view() const {
        return *orb::mem::start_lifetime_as<T>(wire_.data());
    }
};

// The wire payload is exactly one endpoint packet; the bookkeeping tail (length /
// triggered_time / handled) lives after it and is never serialized.
static_assert(sizeof(std::array<std::uint8_t, XBOX_ONE_EP_MAXPKTSIZE>) == XBOX_ONE_EP_MAXPKTSIZE,
              "Xbox wire payload must be exactly XBOX_ONE_EP_MAXPKTSIZE bytes");
static_assert(std::is_trivially_copyable_v<XboxPacket>,
              "XboxPacket moves by memcpy through the tx fifo / OSAL queues");
static_assert(std::is_trivially_default_constructible_v<XboxPacket>,
              "TinyUSB tu_memclr's the driver structs that embed XboxPacket");
static_assert(alignof(XboxPacket) >= 4,
              "XboxPacket must be >=4-byte aligned so data() is word-aligned for the USB DMA "
              "copy inside the CFG_TUSB_MEM_ALIGN epin_buf/epout_buf that embed it");

// --- Packet builders --------------------------------------------------------------------
// `static inline` factories that construct the exact same bytes as the designated-initializer
// blocks they replace in xbox_controller_driver.cpp. They do NOT change power_report_t /
// led_mode_command_t layout -- they just build a value of the existing packed aggregate type.

// CMD_POWER_MODE request carrying a single power-mode byte (device_id 0, type REQUEST).
// `length` is the wire payload size after the frame header == sizeof(data byte) == 1.
static inline power_report_t make_power_report(uint8_t sequence, uint8_t data) {
    power_report_t out = {.frame = {.command = frame_command_e::CMD_POWER_MODE,
                                    .device_id = 0,
                                    .type = frame_type_e::TYPE_REQUEST,
                                    .sequence = sequence,
                                    .length = sizeof(out.data)},
                          .data = data};
    return out;
}

// CMD_LED_MODE request (device_id 0, type REQUEST). The original block omitted `unknown`
// (zero-initialized); we set it to 0 explicitly so the initializer is complete and in
// declaration order (required when this header is included by C++ TUs). Byte-identical to
// the original. `length` is the fixed 3-byte payload (unknown + mode + brightness).
static inline led_mode_command_t make_led_mode_command(uint8_t sequence, led_mode_e mode,
                                                       uint8_t brightness) {
    led_mode_command_t out = {
            .frame =
                    {
                            .command = frame_command_e::CMD_LED_MODE,
                            .device_id = 0,
                            .type = frame_type_e::TYPE_REQUEST,
                            .sequence = sequence,
                            .length = 3,
                    },
            .unknown = 0,
            .mode = mode,
            .brightness = brightness,
    };
    return out;
}

uint8_t xboxp_get_size(const XboxPacket *packet);
uint8_t get_sequence();

void init_packet(XboxPacket *pkt, orb::hal::Clock::time_point time, std::uint8_t length);

void fill_drum_input_from_controller(const XboxPacket *controller_input, XboxPacket *wla_output,
                                     uint8_t player_id);
void fill_guitar_input_from_hid_report(const uint8_t *report, XboxPacket *wla_output,
                                       uint8_t player_id);

#if OPENRB_DEBUG_ENABLED
const char *get_command_name(int cmd);
#endif
