#ifndef ORB_INSTRUMENT_MANAGER_H_
#define ORB_INSTRUMENT_MANAGER_H_

#include "xbox_one_protocol.h"

typedef enum {
    FIRST_INSTRUMENT,
    GUITAR_ONE = FIRST_INSTRUMENT,
    GUITAR_TWO,
    DRUMS,
    N_INSTRUMENTS,
} instruments_e;

void notify_xbox_of_all_instruments(xbox_packet_t *scratch_space);
void notify_xbox_of_single_instrument(instruments_e instrument, xbox_packet_t *scratch_space);
void connect_instrument(instruments_e instrument, xbox_packet_t *scratch_space);
void disconnect_instrument(instruments_e instrument, xbox_packet_t *scratch_space);

#endif