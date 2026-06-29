#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "orb_c_api.h"           // ORB_C_BEGIN/END (impl is C++ now; callers are C)
#include "xbox_one_protocol.h"   // xbox_packet_t

typedef struct { uint8_t data[3]; } midi_note_t;

ORB_C_BEGIN

void app_queues_init(void);

// host TX: core0 producers (device-RX handlers) -> core1 usb_host_task consumer.
bool host_tx_send(const xbox_packet_t *pkt);  // non-blocking; false if full
bool host_tx_recv(xbox_packet_t *pkt);        // non-blocking; false if empty

// MIDI notes from the USB host: core1 producer (drums_read_midi_host) -> core0 drum_task.
bool midi_note_send(const midi_note_t *n);    // non-blocking; false if full
bool midi_note_recv(midi_note_t *n);          // non-blocking; false if empty

ORB_C_END
