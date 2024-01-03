
#include <stdbool.h>
#include <stdint.h>

#include "adapter.h"
#include "bsp/board_api.h"
#include "instruments.h"
#include "packet_queue.h"
#include "usb_midi_host.h"
#include "xbox_one_protocol.h"

typedef enum {
    FIRST_OUT,
    OUT_KICK = FIRST_OUT,
    OUT_PAD_RED,
    OUT_PAD_YELLOW,
    OUT_PAD_BLUE,
    OUT_PAD_GREEN,
    OUT_CYM_YELLOW,
    OUT_CYM_BLUE,
    OUT_CYM_GREEN,
    LAST_OUT = OUT_CYM_GREEN,
    NUM_OUT,
    NO_OUT,
} output_e;

typedef struct {
    uint32_t triggered_at;
    bool     triggered;
} output_state_t;

enum state_flags_t {
    no_flag      = 0,
    changed_flag = (1 << 0),
};
static struct {
    xbox_packet_t input_pkt;
    uint8_t       flags;
} drum_state = {.input_pkt = {.wla_header.playerId = DRUMS,
                              .wla_header.frame =
                                  {
                                      CMD_INPUT,
                                      TYPE_COMMAND,
                                      TYPE_COMMAND,
                                      0,
                                      sizeof(xb_one_drum_input_pkt_t) - sizeof(frame_t),
                                  },
                              .wla_header.unknown = 0x01},
                .flags     = 0};

static uint8_t        midi_dev_addr = 0;
static output_state_t midi_output_states[NUM_OUT];

static output_e get_output_for_note(uint8_t note) {
    switch (note) {
#define MIDI_MAP(midi_note, rb_out) \
    case midi_note:                 \
        return rb_out;
#include "midi_map.h"
#undef MIDI_MAP
        default:
            return NO_OUT;
    }
}

static void update_drum_state_with_midi_input(output_e out, uint8_t state,
                                              xb_one_drum_input_pkt_t *drum_input) {
    switch (out) {
        case OUT_KICK:
            drum_input->kick = state;
            break;
        case OUT_PAD_RED:
            drum_input->pad_red = state;
            break;
        case OUT_PAD_BLUE:
            drum_input->pad_blue = state;
            break;
        case OUT_PAD_GREEN:
            drum_input->pad_green = state;
            break;
        case OUT_PAD_YELLOW:
            drum_input->pad_yellow = state;
            break;
        case OUT_CYM_YELLOW:
            drum_input->cymbal_yellow = state;
            break;
        case OUT_CYM_BLUE:
            drum_input->cymbal_blue = state;
            break;
        case OUT_CYM_GREEN:
            drum_input->cymbal_green = state;
            break;
        default:
            break;
    }
    return;
}

void note_on(uint8_t note, uint8_t velocity) {
    OPENRB_DEBUG("Note On: %d %d\n", note, velocity);

    if (velocity <= VELOCITY_THRESH)
        return;
    output_e out = get_output_for_note(note);
    if (out == NO_OUT)
        return;

    if (midi_output_states[out].triggered)
        return;

    update_drum_state_with_midi_input(out, 1, &drum_state.input_pkt.drum_input);
    drum_state.flags |= changed_flag;

    midi_output_states[out].triggered    = true;
    midi_output_states[out].triggered_at = board_millis();
    return;
}
typedef enum {
    InvalidType          = 0x00,             ///< For notifying errors
    NoteOff              = 0x80,             ///< Channel Message - Note Off
    NoteOn               = 0x90,             ///< Channel Message - Note On
    AfterTouchPoly       = 0xA0,             ///< Channel Message - Polyphonic AfterTouch
    ControlChange        = 0xB0,             ///< Channel Message - Control Change / Channel Mode
    ProgramChange        = 0xC0,             ///< Channel Message - Program Change
    AfterTouchChannel    = 0xD0,             ///< Channel Message - Channel (monophonic) AfterTouch
    PitchBend            = 0xE0,             ///< Channel Message - Pitch Bend
    SystemExclusive      = 0xF0,             ///< System Exclusive
    SystemExclusiveStart = SystemExclusive,  ///< System Exclusive Start
    TimeCodeQuarterFrame = 0xF1,             ///< System Common - MIDI Time Code Quarter Frame
    SongPosition         = 0xF2,             ///< System Common - Song Position Pointer
    SongSelect           = 0xF3,             ///< System Common - Song Select
    Undefined_F4         = 0xF4,
    Undefined_F5         = 0xF5,
    TuneRequest          = 0xF6,  ///< System Common - Tune Request
    SystemExclusiveEnd   = 0xF7,  ///< System Exclusive End
    Clock                = 0xF8,  ///< System Real Time - Timing Clock
    Undefined_F9         = 0xF9,
    Tick          = Undefined_F9,  ///< System Real Time - Timing Tick (1 tick = 10 milliseconds)
    Start         = 0xFA,          ///< System Real Time - Start
    Continue      = 0xFB,          ///< System Real Time - Continue
    Stop          = 0xFC,          ///< System Real Time - Stop
    Undefined_FD  = 0xFD,
    ActiveSensing = 0xFE,  ///< System Real Time - Active Sensing
    SystemReset   = 0xFF,  ///< System Real Time - System Reset
} midi_type_e;

extern volatile adapter_state_t adapter_state;

static midi_type_e get_type_from_status(uint8_t inStatus) {
    if ((inStatus < 0x80) || (inStatus == Undefined_F4) || (inStatus == Undefined_F5) ||
        (inStatus == Undefined_FD))
        return InvalidType;  // Data bytes and undefined.

    if (inStatus < 0xf0)
        // Channel message, remove channel nibble.
        return inStatus & 0xf0;

    return inStatus;
}

static uint8_t pending_msg[48];
static uint32_t last_sent_time = 0; 
void drum_task() {
    if (adapter_state != STATE_RUNNING)
        return;

    uint8_t     cable_num;
    uint32_t    extracted;
    midi_type_e type;
    while (1) {
        extracted =
            tuh_midi_stream_read(midi_dev_addr, &cable_num, pending_msg, sizeof(pending_msg));
        if (extracted == 0)
            break;

        type = get_type_from_status(pending_msg[0]);
        if (type == NoteOn)
            note_on(pending_msg[1], pending_msg[2]);
    }

    uint32_t current_time = board_millis();
    for (int out = 0; out < NUM_OUT; out++) {
        if (!midi_output_states[out].triggered) {
            continue;
        }

        uint32_t time_since_trigger = current_time - midi_output_states[out].triggered_at;
        if (time_since_trigger > TRIGGER_HOLD_MS) {
            update_drum_state_with_midi_input(out, 0, &drum_state.input_pkt.drum_input);
            midi_output_states[out].triggered = false;
            drum_state.flags |= changed_flag;
        }
    }

    if (drum_state.flags & changed_flag && current_time - last_sent_time > ADAPTER_OUT_INTERVAL) {
        drum_state.input_pkt.frame.sequence = get_sequence();
        drum_state.input_pkt.triggered_time = current_time;
        drum_state.input_pkt.length         = sizeof(xb_one_drum_input_pkt_t);
        xbox_fifo_write(&drum_state.input_pkt);
        drum_state.flags &= ~changed_flag;
        last_sent_time = current_time;
    }
}

void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep, uint8_t num_cables_rx,
                       uint16_t num_cables_tx) {
    OPENRB_DEBUG(
        "MIDI device address = %u, IN endpoint %u has %u cables, OUT endpoint %u has %u cables\r\n",
        dev_addr, in_ep & 0xf, num_cables_rx, out_ep & 0xf, num_cables_tx);

    if (midi_dev_addr == 0) {
        // then no MIDI device is currently connected
        midi_dev_addr = dev_addr;
    } else {
        OPENRB_DEBUG(
            "A different USB MIDI Device is already connected.\r\nOnly one device at a time is "
            "supported in this program\r\nDevice is disabled\r\n");
    }
}

// Invoked when device with hid interface is un-mounted
void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance) {
    if (dev_addr == midi_dev_addr) {
        midi_dev_addr = 0;
        OPENRB_DEBUG("MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr,
                     instance);
    } else {
        OPENRB_DEBUG("Unused MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr,
                     instance);
    }
}

void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets) {
    if (midi_dev_addr != dev_addr)
        return;
    if (num_packets == 0)
        return;

    uint8_t     cable_num;
    uint32_t    extracted;
    midi_type_e type;
    while (1) {
        extracted = tuh_midi_stream_read(dev_addr, &cable_num, pending_msg, sizeof(pending_msg));
        if (extracted == 0)
            return;

        type = get_type_from_status(pending_msg[0]);
        if (type == NoteOn)
            note_on(pending_msg[1], pending_msg[2]);
    }
}