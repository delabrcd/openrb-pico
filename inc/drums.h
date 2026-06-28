#ifndef ORB_DRUMS_H_
#define ORB_DRUMS_H_

void drum_task();

// Runs on core1 (owns the USB host stack): drains the USB-host MIDI FIFO and pushes
// each parsed message onto the MIDI note queue for drum_task (core0) to consume.
void drums_read_midi_host(void);

#endif
