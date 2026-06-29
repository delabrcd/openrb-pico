/*
 * Inter-task / inter-core queues, now expressed with the orb::osal::Queue<> RAII wrapper
 * (inc/osal/queue.hpp) instead of hand-rolled storage+control-block+handle triples.
 * The free-function API (declared in app_queues.h) is unchanged; every caller
 * (drums.cpp, main.cpp) is a C++ TU.
 */
#include "app_queues.h"

#include "osal/queue.hpp"

// host TX queue: core0 device-RX handlers enqueue, core1 usb_host_task dequeues and
// submits the actual USB host transfer.
static orb::osal::Queue<xbox_packet_t, 8> s_host_tx;

// MIDI note queue: core1 drums_read_midi_host enqueues parsed notes, core0 drum_task
// dequeues and feeds them into the drum state.
static orb::osal::Queue<midi_note_t, 32> s_midi_note;

void app_queues_init(void) {
    s_host_tx.create();
    s_midi_note.create();
}

bool host_tx_send(const xbox_packet_t *pkt) { return s_host_tx.send(*pkt); }
bool host_tx_recv(xbox_packet_t *pkt) { return s_host_tx.recv(*pkt); }

bool midi_note_send(const midi_note_t *n) { return s_midi_note.send(*n); }
bool midi_note_recv(midi_note_t *n) { return s_midi_note.recv(*n); }
