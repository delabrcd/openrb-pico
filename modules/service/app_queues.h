#pragma once
#include <cstdint>
#include "xbox_one_protocol.h"   // xbox_packet_t

typedef struct { uint8_t data[3]; } midi_note_t;

// Plain C++ free functions (every consumer is a C++ TU); the queues themselves are owned by
// orb::app::System, so these forward into it -- defined in the app-layer composition-root
// bridge, modules/app/system.cpp (service/ TUs never reach into orb::app directly).
void app_queues_init(void);

// host TX: core0 producers (device-RX handlers) -> core1 usb_host_task consumer.
bool host_tx_send(const xbox_packet_t *pkt);  // non-blocking; false if full
bool host_tx_recv(xbox_packet_t *pkt);        // non-blocking; false if empty
