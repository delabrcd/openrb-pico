#include "drums.h"

#include <stdbool.h>
#include <stdint.h>

#include "adapter.h"
#include "bsp/board_api.h"
#include "instrument_manager.h"
#include "midi.h"
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
    bool triggered;
} output_state_t;

enum state_flags_t {
    no_flag = 0,
    changed_flag = (1 << 0),
};

static struct {
    xbox_packet_t input_pkt;
    uint8_t midi_dev_addr;
    output_state_t midi_output_states[NUM_OUT];
    uint8_t flags;
} drum_state = {.midi_dev_addr = 0,
                .input_pkt = {.wla_header.playerId = DRUMS,
                              .wla_header.frame =
                                      {
                                              CMD_INPUT,
                                              TYPE_COMMAND,
                                              TYPE_COMMAND,
                                              0,
                                              sizeof(xb_one_drum_input_pkt_t) - sizeof(frame_t),
                                      },
                              .wla_header.unknown = 0x01},
                .flags = 0};

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

static void note_on(uint8_t note, uint8_t velocity) {
    if (velocity <= VELOCITY_THRESH) return;

    output_e out = get_output_for_note(note);
    if (out == NO_OUT) return;

    if (drum_state.midi_output_states[out].triggered) return;

    update_drum_state_with_midi_input(out, 1, &drum_state.input_pkt.drum_input);
    drum_state.flags |= changed_flag;

    OPENRB_DEBUG("NOTE ON: %d %d\r\n", out, velocity);

    drum_state.midi_output_states[out].triggered = true;
    drum_state.midi_output_states[out].triggered_at = board_millis();
    return;
}

extern volatile adapter_state_t adapter_state;

static midi_type_e get_type_from_status(uint8_t status) {
    if ((status < 0x80) || (status == Undefined_F4) || (status == Undefined_F5) ||
        (status == Undefined_FD))
        return InvalidType;  // Data bytes and undefined.

    if (status < 0xf0)
        // Channel message, remove channel nibble.
        return status & 0xf0;

    return status;
}

void drum_task() {
    if (adapter_state != STATE_RUNNING) return;

    static uint8_t pending_msg[48];
    static uint8_t cable_num;
    static midi_type_e type;
    static uint32_t current_time;

    while (tuh_midi_stream_read(drum_state.midi_dev_addr, &cable_num, pending_msg,
                                sizeof(pending_msg)) != 0) {
        type = get_type_from_status(pending_msg[0]);
        if (type == NoteOn) note_on(pending_msg[1], pending_msg[2]);
    }

    while (serial_midi_read(pending_msg)) {
        type = get_type_from_status(pending_msg[0]);
        if (type == NoteOn) note_on(pending_msg[1], pending_msg[2]);
    }

    current_time = board_millis();
    for (int out = 0; out < NUM_OUT; out++) {
        if (!drum_state.midi_output_states[out].triggered) {
            continue;
        }

        uint32_t time_since_trigger =
                current_time - drum_state.midi_output_states[out].triggered_at;
        if (time_since_trigger > TRIGGER_HOLD_MS) {
            OPENRB_DEBUG("NOTE OFF: %d\r\n", out);
            update_drum_state_with_midi_input(out, 0, &drum_state.input_pkt.drum_input);
            drum_state.midi_output_states[out].triggered = false;
            drum_state.flags |= changed_flag;
        }
    }

    if (drum_state.flags & changed_flag &&
        current_time - drum_state.input_pkt.triggered_time > ADAPTER_OUT_INTERVAL) {
        init_packet(&drum_state.input_pkt, current_time, sizeof(xb_one_drum_input_pkt_t));
        xbox_fifo_write(&drum_state.input_pkt);
        drum_state.flags &= ~changed_flag;
    }
}

void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep, uint8_t num_cables_rx,
                       uint16_t num_cables_tx) {
    (void)in_ep;
    (void)out_ep;
    (void)num_cables_rx;
    (void)num_cables_tx;

    OPENRB_DEBUG(
            "MIDI device address = %u, IN endpoint %u has %u cables, OUT endpoint %u has %u "
            "cables\r\n",
            dev_addr, in_ep & 0xf, num_cables_rx, out_ep & 0xf, num_cables_tx);

    if (drum_state.midi_dev_addr == 0) {
        // then no MIDI device is currently connected
        drum_state.midi_dev_addr = dev_addr;
        connect_instrument(DRUMS, &drum_state.input_pkt);
    } else {
        OPENRB_DEBUG(
                "A different USB MIDI Device is already connected.\r\nOnly one device at a time is "
                "supported in this program\r\nDevice is disabled\r\n");
    }
}

// Invoked when device with hid interface is un-mounted
void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)instance;

    if (dev_addr == drum_state.midi_dev_addr) {
        drum_state.midi_dev_addr = 0;
        OPENRB_DEBUG("MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr,
                     instance);
        disconnect_instrument(DRUMS, &drum_state.input_pkt);
    } else {
        OPENRB_DEBUG("Unused MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr,
                     instance);
    }
}

void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets) {
    (void)dev_addr;
    (void)num_packets;
}