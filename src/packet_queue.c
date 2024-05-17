
#include "common/tusb_common.h"

#include "tusb_option.h"
#include "xbox_one_protocol.h"

// clang-format off
#include "osal/osal.h"
#include "common/tusb_fifo.h"
// clang-format on

#define XBOX_FIFO_SIZE 16

static struct {
    tu_fifo_t        fifo;
    xbox_packet_t    buffer[XBOX_FIFO_SIZE];
    osal_mutex_def_t wr_mutex;
    osal_mutex_def_t rd_mutex;
} fifo;

void xbox_fifo_init() {
    tu_fifo_config(&fifo.fifo, fifo.buffer, XBOX_FIFO_SIZE, sizeof(xbox_packet_t), false);
    tu_fifo_config_mutex(&fifo.fifo, osal_mutex_create(&fifo.wr_mutex), NULL);
}

uint32_t xbox_fifo_read(xbox_packet_t *buffer) {
    return tu_fifo_read(&fifo.fifo, buffer);
}

uint32_t xbox_fifo_peek(xbox_packet_t *buffer) {
    return tu_fifo_peek(&fifo.fifo, buffer);
}

void xbox_fifo_advance() {
    return tu_fifo_advance_read_pointer(&fifo.fifo, 1);
}

uint32_t xbox_fifo_write(const xbox_packet_t *buffer) {
    return tu_fifo_write(&fifo.fifo, buffer);
}

uint32_t xbox_fifo_count() {
    return tu_fifo_count(&fifo.fifo);
}

bool xbox_fifo_empty() {
    return tu_fifo_empty(&fifo.fifo);
}

bool xbox_fifo_full() {
    return tu_fifo_full(&fifo.fifo);
}

void xbox_fifo_clear() {
    tu_fifo_clear(&fifo.fifo);
}