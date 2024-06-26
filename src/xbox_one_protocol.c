;
#include "xbox_one_protocol.h"

#include <string.h>

#include "orb_debug.h"

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
        case CMD_ADD_PLAYER:
            return "CMD_ADD_PLAYER";
        case CMD_DROP_PLAYER:
            return "CMD_DROP_PLAYER";
        case CMD_AUDIO_SAMPLES:
            return "CMD_AUDIO_SAMPLES";
        default:
            return "Unknown CMD";
    }
    return "Unknown CMD";
}
#endif
