#ifndef XBOX_ONE_PROTOCOL_H
#define XBOX_ONE_PROTOCOL_H

#include <assert.h>
#include <stdint.h>

#include "orb_debug.h"

enum frame_command_e {
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

enum frame_type_e {
    TYPE_COMMAND = 0x00,
    TYPE_ACK = 0x01,
    TYPE_REQUEST = 0x02,
};

enum power_mode_e {
    POWER_ON = 0x00,
    POWER_SLEEP = 0x01,
    POWER_OFF = 0x04,
};

enum led_mode_e {
    LED_OFF = 0x00,
    LED_ON = 0x01,
    LED_BLINK_FAST = 0x02,
    LED_BLINK_MED = 0x03,
    LED_BLINK_SLOW = 0x04,
    LED_FADE_SLOW = 0x08,
    LED_FADE_FAST = 0x09,
};

typedef uint8_t led_mode_t;

/* Xbox One data taken from descriptors */
#define XBOX_ONE_EP_MAXPKTSIZE 64  // Max size for data via USB

typedef struct {
    uint8_t command;
    uint8_t device_id : 4;
    uint8_t type : 4;
    uint8_t sequence;
    uint8_t length;
} __attribute__((packed)) frame_t;

typedef union {
    struct parsed_data {
        frame_t frame;
        uint8_t data;
    } __attribute__((packed)) data;

    uint8_t buffer[sizeof(struct parsed_data)];
} power_report_t;

typedef struct {
    frame_t frame;
    uint8_t unknown;
    led_mode_t mode;
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

typedef struct {
    union {
        frame_t frame;
        xb_one_wireless_legacy_adapter_pkt_t wla_header;
        power_report_t power;
        xb_one_controller_input_pkt_t controller_input;
        xb_one_drum_input_pkt_t drum_input;
        xb_one_guitar_input_pkt_t guitar_input;

        uint8_t buffer[XBOX_ONE_EP_MAXPKTSIZE];
    };
    uint8_t length;
    uint32_t triggered_time;
    uint8_t handled;
} __attribute__((packed)) xbox_packet_t;

// static_assert(sizeof(xbox_packet_t) == XBOX_ONE_EP_MAXPKTSIZE, "Incorrect Xbox Packet Size");

uint8_t xboxp_get_size(const xbox_packet_t *packet);
uint8_t get_sequence();

void init_packet(xbox_packet_t *pkt, uint32_t time, uint8_t length);

void fill_drum_input_from_controller(const xbox_packet_t *controller_input,
                                     xbox_packet_t *wla_output, uint8_t player_id);
void fill_guitar_input_from_hid_report(const uint8_t *report, xbox_packet_t *wla_output,
                                       uint8_t player_id);

#if OPENRB_DEBUG_ENABLED
const char *get_command_name(int cmd);
#endif

#endif