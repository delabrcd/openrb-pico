#ifndef ADAPTER_H
#define ADAPTER_H

typedef enum adapter_state_e {
    STATE_NONE,
    STATE_INIT,
    STATE_IDENTIFYING,
    STATE_AUTHENTICATING,
    STATE_RUNNING,
    STATE_POWER_OFF,
} adapter_state_t;

#endif  // ADAPTER_H