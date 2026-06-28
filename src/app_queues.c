#include "app_queues.h"

#include "FreeRTOS.h"
#include "queue.h"

// host TX queue: core0 device-RX handlers enqueue, core1 usb_host_task dequeues and
// submits the actual USB host transfer. Static allocation (configSUPPORT_STATIC_ALLOCATION).
#define HOST_TX_LEN 8
static uint8_t host_tx_storage[HOST_TX_LEN * sizeof(xbox_packet_t)];
static StaticQueue_t host_tx_q_buf;
static QueueHandle_t host_tx_q;

// MIDI note queue: core1 drums_read_midi_host enqueues parsed notes, core0 drum_task
// dequeues and feeds them into the drum state.
#define MIDI_NOTE_LEN 32
static uint8_t midi_note_storage[MIDI_NOTE_LEN * sizeof(midi_note_t)];
static StaticQueue_t midi_note_q_buf;
static QueueHandle_t midi_note_q;

void app_queues_init(void) {
    host_tx_q = xQueueCreateStatic(HOST_TX_LEN, sizeof(xbox_packet_t), host_tx_storage,
                                   &host_tx_q_buf);
    midi_note_q = xQueueCreateStatic(MIDI_NOTE_LEN, sizeof(midi_note_t), midi_note_storage,
                                     &midi_note_q_buf);
}

bool host_tx_send(const xbox_packet_t *pkt) {
    return xQueueSend(host_tx_q, pkt, 0) == pdTRUE;
}

bool host_tx_recv(xbox_packet_t *pkt) {
    return xQueueReceive(host_tx_q, pkt, 0) == pdTRUE;
}

bool midi_note_send(const midi_note_t *n) {
    return xQueueSend(midi_note_q, n, 0) == pdTRUE;
}

bool midi_note_recv(midi_note_t *n) {
    return xQueueReceive(midi_note_q, n, 0) == pdTRUE;
}
