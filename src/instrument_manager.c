
#include "instrument_manager.h"

#include <pico/platform.h>
#include <stdint.h>
#include <string.h>

#include "adapter.h"
#include "orb_debug.h"
#include "packet_queue.h"
#include "util.h"
#include "xbox_one_protocol.h"

#if OPENRB_DEBUG_ENABLED
const char *instrument_names[N_INSTRUMENTS] = {"GUITAR_ONE", "GUITAR_TWO", "DRUMS"};
#endif

extern volatile adapter_state_t adapter_state;

static volatile uint8_t connected_instruments[N_INSTRUMENTS] = {0, 0, 0};

const uint8_t __in_flash() instrument_notify[N_INSTRUMENTS][22] = {
    {0x22, 0x00, 0x00, 0x12, 0x00, 0x01, 0x14, 0x30, 0x00, 0x87, 0x67,
     0x00, 0x75, 0x00, 0x69, 0x00, 0x74, 0x00, 0x61, 0x00, 0x72, 0x00},
    {0x22, 0x00, 0x00, 0x12, 0x01, 0x01, 0x14, 0x30, 0x00, 0x87, 0x67,
     0x00, 0x75, 0x00, 0x69, 0x00, 0x74, 0x00, 0x61, 0x00, 0x72, 0x00},
    {0x22, 0x00, 0x00, 0x12, 0x02, 0x01, 0x1b, 0xad, 0x00, 0x88, 0x64,
     0x00, 0x72, 0x00, 0x75, 0x00, 0x6D, 0x00, 0x73, 0x00, 0x00, 0x00}
};

const uint8_t __in_flash() instrument_drop_out[N_INSTRUMENTS][22] = {
    {0x23, 0x00, 0x00, 0x01, 0x00, 0xFF, 0x05},
    {0x23, 0x00, 0x00, 0x01, 0x01, 0xFF, 0x05},
    {0x23, 0x00, 0x00, 0x01, 0x02, 0xFF, 0x05},
};

static inline void grab_packet(xbox_packet_t *pkt, instruments_e instrument, bool connect) {
    const uint8_t *local;
    uint32_t size;
    if (connect) {
        local = instrument_notify[instrument];
        size = UTIL_NUM(instrument_notify[instrument]);
    } else {
        local = instrument_drop_out[instrument];
        size = UTIL_NUM(instrument_drop_out[instrument]);
    }

    memcpy(pkt->buffer, local, size);
    init_packet(pkt, 0, size);
}

void notify_xbox_of_all_instruments(xbox_packet_t *scratch_space) {
    OPENRB_DEBUG("notify_xbox_of_all_instruments\r\n");
    for (int i = FIRST_INSTRUMENT; i < N_INSTRUMENTS; i++) {
        if (!connected_instruments[i]) continue;
        grab_packet(scratch_space, i, true);
        xbox_fifo_write(scratch_space);
    }
}

void notify_xbox_of_single_instrument(instruments_e instrument, xbox_packet_t *scratch_space) {
    OPENRB_DEBUG("notify_xbox_of_single_instrument: %d", instrument);
    if (instrument < N_INSTRUMENTS) {
        OPENRB_DEBUG(" - %s\r\n", instrument_names[instrument]);
        grab_packet(scratch_space, instrument, true);
        xbox_fifo_write(scratch_space);
    }
}

void connect_instrument(instruments_e instrument, xbox_packet_t *scratch_space) {
    if (connected_instruments[instrument]) return;
    OPENRB_DEBUG("%s connected!\r\n", instrument_names[instrument]);
    connected_instruments[instrument] = 1;

    if (adapter_state != STATE_RUNNING) return;

    grab_packet(scratch_space, instrument, true);
    xbox_fifo_write(scratch_space);
}

void disconnect_instrument(instruments_e instrument, xbox_packet_t *scratch_space) {
    if (!connected_instruments[instrument]) return;
    OPENRB_DEBUG("%s disconnected!\r\n", instrument_names[instrument]);
    connected_instruments[instrument] = 0;

    if (adapter_state != STATE_RUNNING) return;

    grab_packet(scratch_space, instrument, false);
    xbox_fifo_write(scratch_space);
}