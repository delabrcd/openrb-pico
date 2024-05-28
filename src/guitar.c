
#include <stdint.h>

// clang-format off
#include "tusb.h" // IWYU pragma: export 
#include "class/hid/hid_host.h"
// clang-format on
#include "instrument_manager.h"
#include "orb_debug.h"
#include "packet_queue.h"
#include "xbox_one_protocol.h"

#define VENDOR(vid, ...)      \
    case vid: {               \
        switch (pid) {        \
            __VA_ARGS__       \
            default:          \
                return false; \
        }                     \
        break;                \
    }

#define PRODUCT(pid) \
    case pid:        \
        return true;
typedef struct guitar_data {
    xbox_packet_t out_packet;
    uint8_t dev_addr;
} guitar_data_t;

static guitar_data_t guitar_1_data = {{}, 0};
static guitar_data_t guitar_2_data = {{}, 0};

bool is_hid_guitar(uint8_t dev_addr) {
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    switch (vid) { VENDOR(0x1209, PRODUCT(0x2882)); }
    return false;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report,
                      uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    OPENRB_DEBUG("HID device address = %d, instance = %d is mounted\r\n", dev_addr, instance);
    OPENRB_DEBUG("VID = %04x, PID = %04x\r\n", vid, pid);

    if (is_hid_guitar(dev_addr)) {
        OPENRB_DEBUG("GUITAR is in supported list\r\n");
        if (guitar_1_data.dev_addr == 0) {
            connect_instrument(GUITAR_ONE, &guitar_1_data.out_packet);
            guitar_1_data.dev_addr = dev_addr;
        } else if (guitar_2_data.dev_addr == 0) {
            connect_instrument(GUITAR_TWO, &guitar_2_data.out_packet);
            guitar_2_data.dev_addr = dev_addr;
        } else {
            OPENRB_DEBUG("Already have 2 guitars connected, can't add another...\r\n")
            return;
        }

        // we need to request the first report
        if (!tuh_hid_receive_report(dev_addr, instance)) {
            OPENRB_DEBUG("Error: cannot request to receive report\r\n");
        }
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx) {
    (void)idx;
    if (dev_addr == guitar_1_data.dev_addr) {
        disconnect_instrument(GUITAR_ONE, &guitar_1_data.out_packet);
        guitar_1_data.dev_addr = 0;

    } else if (dev_addr == guitar_2_data.dev_addr) {
        disconnect_instrument(GUITAR_TWO, &guitar_2_data.out_packet);
        guitar_2_data.dev_addr = 0;
    }
}

// static int test = 0;
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report,
                                uint16_t len) {
    OPENRB_DEBUG("Report Received\r\n");
    OPENRB_DEBUG_BUF(report, len);
    OPENRB_DEBUG("\r\n");

    xbox_packet_t *packet = NULL;
    uint8_t player_number;
    // continue to request to receive report
    if (guitar_1_data.dev_addr == dev_addr) {
        packet = &guitar_1_data.out_packet;
        player_number = GUITAR_ONE;
    } else if (guitar_2_data.dev_addr == dev_addr) {
        packet = &guitar_2_data.out_packet;
        player_number = GUITAR_TWO;
    } else {
        return;
    }

    fill_guitar_input_from_hid_report(report, packet, player_number);
    xbox_fifo_write(packet);

    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("Error: cannot request to receive report\r\n");
    }
}