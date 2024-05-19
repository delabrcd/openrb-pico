#ifndef ORB_INSTRUMENT_MANAGER_H_
#define ORB_INSTRUMENT_MANAGER_H_

typedef enum {
    FIRST_INSTRUMENT,
    GUITAR_ONE = FIRST_INSTRUMENT,
    GUITAR_TWO,
    DRUMS,
    N_INSTRUMENTS,
} instruments_e;

void notify_xbox_of_all_instruments();
void notify_xbox_of_single_instrumenty(instruments_e instrument);
void connect_instrument(instruments_e instrument);
void disconnect_instrument(instruments_e instrument);

#endif