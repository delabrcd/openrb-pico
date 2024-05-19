#ifndef ORB_GENERIC_FIFO_H_
#define ORB_GENERIC_FIFO_H_

// clang-format off
#include <osal/osal.h>
#include <common/tusb_fifo.h>
// clang-format on

#define GENERIC_FIFO_EXPORTS(name, type)            \
    void name##_fifo_init();                        \
    uint32_t name##_fifo_read(type *buffer);        \
    uint32_t name##_fifo_peek(type *buffer);        \
    void name##_fifo_advance();                     \
    uint32_t name##_fifo_write(const type *buffer); \
    uint32_t name##_fifo_count();                   \
    bool name##_fifo_empty();                       \
    bool name##_fifo_full();                        \
    void name##_fifo_clear();

#define CREATE_GENERIC_FIFO(name, type, size, rd_mtx, wr_mtx)                                    \
    static struct {                                                                              \
        tu_fifo_t fifo;                                                                          \
        type buffer[size];                                                                       \
        osal_mutex_def_t wr_mutex;                                                               \
        osal_mutex_def_t rd_mutex;                                                               \
    } fifo;                                                                                      \
                                                                                                 \
    void name##_fifo_init() {                                                                    \
        tu_fifo_config(&fifo.fifo, fifo.buffer, size, sizeof(type), false);                      \
        osal_mutex_t write_mtx = NULL;                                                           \
        if (wr_mtx) {                                                                            \
            write_mtx = osal_mutex_create(&fifo.wr_mutex);                                       \
        }                                                                                        \
        osal_mutex_t read_mtx = NULL;                                                            \
        if (rd_mtx) {                                                                            \
            read_mtx = osal_mutex_create(&fifo.rd_mutex);                                        \
        }                                                                                        \
        tu_fifo_config_mutex(&fifo.fifo, write_mtx, read_mtx);                                   \
    }                                                                                            \
    uint32_t name##_fifo_read(type *buffer) { return tu_fifo_read(&fifo.fifo, buffer); }         \
    uint32_t name##_fifo_peek(type *buffer) { return tu_fifo_peek(&fifo.fifo, buffer); }         \
    void name##_fifo_advance() { return tu_fifo_advance_read_pointer(&fifo.fifo, 1); }           \
    uint32_t name##_fifo_write(const type *buffer) { return tu_fifo_write(&fifo.fifo, buffer); } \
    uint32_t name##_fifo_count() { return tu_fifo_count(&fifo.fifo); }                           \
    bool name##_fifo_empty() { return tu_fifo_empty(&fifo.fifo); }                               \
    bool name##_fifo_full() { return tu_fifo_full(&fifo.fifo); }                                 \
    void name##_fifo_clear() { tu_fifo_clear(&fifo.fifo); }

#endif