#include "packet_queue.h"  // IWYU pragma: export

#define XBOX_FIFO_SIZE 16

CREATE_GENERIC_FIFO(xbox, xbox_packet_t, XBOX_FIFO_SIZE, true, true)
