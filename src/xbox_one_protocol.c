#include "bsp/board_api.h"
;
#include <string.h>

#include "orb_debug.h"
#include "xbox_one_protocol.h"

static uint8_t sequence = 0;

uint8_t get_sequence() { return sequence++; }

uint8_t xboxp_get_size(const xbox_packet_t *packet) {
    if (!packet) return 0;
    return packet->length;
}

void init_packet(xbox_packet_t *pkt, uint32_t time, uint8_t length) {
    pkt->frame.sequence = get_sequence();
    pkt->triggered_time = time;
    pkt->handled = 0;
    pkt->length = length;
}

typedef struct hid_input_report_t {
    uint8_t cmd_id : 8;  // 00

    // 00
    uint8_t green : 1;   // Green button (bit 0)
    uint8_t red : 1;     // Red button (bit 1)
    uint8_t yellow : 1;  // Yellow button (bit 2)
    uint8_t blue : 1;    // Blue button (bit 3)
    uint8_t orange : 1;  // Orange button (bit 4)
    uint8_t unknown : 1;
    uint8_t select : 1;  // Select button (bit 7)
    uint8_t start : 1;   // Start button (bit 8)

    // 00
    uint8_t dpad_maybe : 8;

    // 00
    // strum up -       00
    // strum down -     04
    // strum center -   08
    uint8_t strum_bits : 8;

    uint8_t whammy_bits : 8;
    uint8_t dunno1 : 8;
    uint8_t tilt_bits : 8;
} __attribute__((packed)) hid_input_report_t;

// static void fill_wla_header()
#define MAKE_COLORED_STATE(report) \
    ((report->blue << 2) | (report->green << 0) | (report->red << 1) | (report->yellow << 3));

#define MAKE_DPAD_STATE(report)        \
    (report->strum_bits == 0x08 ? 0x00 \
                                : (report->strum_bits & 0x04 << 1) | (report->strum_bits & 0x0))

void fill_guitar_input_from_hid_report(const uint8_t *report, xbox_packet_t *wla_output,
                                       uint8_t player_id) {
    init_packet(wla_output, board_millis(), sizeof(xb_one_guitar_input_pkt_t));

    wla_output->frame.command = CMD_INPUT;
    wla_output->frame.deviceId = 0;
    wla_output->frame.type = 0;
    wla_output->frame.sequence = get_sequence();
    wla_output->frame.length = sizeof(xb_one_guitar_input_pkt_t) - sizeof(frame_t);
    wla_output->wla_header.playerId = player_id;

    xb_one_guitar_input_pkt_t *guitar_pkt = &wla_output->guitar_input;
    const hid_input_report_t *hid_report = (const hid_input_report_t *)report;

    guitar_pkt->wla_header.coloredButtonState1 = MAKE_COLORED_STATE(hid_report);
    guitar_pkt->coloredButtonState2 = guitar_pkt->wla_header.coloredButtonState1;
    guitar_pkt->orangeButton = hid_report->orange;
    guitar_pkt->startButton = hid_report->start;
    guitar_pkt->selectButton = hid_report->select | (hid_report->tilt_bits > 128);
    guitar_pkt->whammy = hid_report->whammy_bits;

    if (hid_report->strum_bits == 0x00) {
        guitar_pkt->dpadState2 = (1 << 0);
    } else if (hid_report->strum_bits & 0x04) {
        guitar_pkt->dpadState2 = (1 << 1);
    } else if (hid_report->strum_bits & 0x08) {
        guitar_pkt->dpadState2 = 0;
    }
    guitar_pkt->wla_header.dpadState1 = guitar_pkt->dpadState2;
    return;
}

void fill_drum_input_from_controller(const xbox_packet_t *controller_input,
                                     xbox_packet_t *wla_output, uint8_t player_id) {
    memset(wla_output->buffer, 0, sizeof(wla_output->buffer));

    wla_output->handled = 0;
    wla_output->triggered_time = 0;

    wla_output->length = sizeof(xb_one_drum_input_pkt_t);
    wla_output->frame.command = controller_input->frame.command;
    wla_output->frame.deviceId = controller_input->frame.deviceId;
    wla_output->frame.type = controller_input->frame.deviceId;
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
    return;
}

#if OPENRB_DEBUG_ENABLED
const char *get_command_name(int cmd) {
    switch (cmd) {
        case CMD_ACKNOWLEDGE:
            return "CMD_ACK";
        case CMD_ANNOUNCE:
            return "CMD_ANNOUNCE";
        case CMD_STATUS:
            return "CMD_STATUS";
        case CMD_IDENTIFY:
            return "CMD_IDENTIFY";
        case CMD_POWER_MODE:
            return "CMD_POWER_MODE";
        case CMD_AUTHENTICATE:
            return "CMD_AUTHENTICATE";
        case CMD_GUIDE_BTN:
            return "CMD_GUIDE_BTN";
        case CMD_AUDIO_CONFIG:
            return "CMD_AUDIO_CONFIG";
        case CMD_RUMBLE:
            return "CMD_RUMBLE";
        case CMD_LED_MODE:
            return "CMD_LED_MODE";
        case CMD_SERIAL_NUM:
            return "CMD_SERIAL_NUM";
        case CMD_INPUT:
            return "CMD_INPUT";
        case CMD_LIST_INSTRUMENT:
            return "CMD_LIST_INSTRUMENT";
        case CMD_ADD_PLAYER:
            return "CMD_ADD_PLAYER";
        case CMD_DROP_PLAYER:
            return "CMD_DROP_PLAYER";
        case CMD_LIST_CONNECTED_INSTRUMENTS:
            return "CMD_LIST_CONNECTED_INSTRUMENTS";
        case CMD_AUDIO_SAMPLES:
            return "CMD_AUDIO_SAMPLES";
        default:
            return "Unknown CMD";
    }
    return "Unknown CMD";
}
#endif
