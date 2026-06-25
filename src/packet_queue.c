#include "packet_queue.h"  // IWYU pragma: export

#define XBOX_FIFO_SIZE 16

// single reader (core0 xboxd_send_task) -> no read mutex needed; writers on both
// cores keep the write mutex. tinyusb 0.18 made the osal_pico mutex a real blocking
// pico mutex, so the redundant read mutex caused a permanent deadlock.
CREATE_GENERIC_FIFO(xbox, xbox_packet_t, XBOX_FIFO_SIZE, false, true)
