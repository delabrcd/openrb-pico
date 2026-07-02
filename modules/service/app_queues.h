#pragma once
#include <cstdint>
#include "xbox_one_protocol.h"   // XboxPacket

// Cross-core queue payload (host-TX core0->core1, MIDI notes core1->core0). The queues
// themselves are owned by orb::app::System (orb::app::system().host_tx() /
// orb::app::system().midi_notes()) -- host TX: core0 producers (device-RX handlers) ->
// core1 usb_host_task consumer.
typedef struct { uint8_t data[3]; } midi_note_t;
