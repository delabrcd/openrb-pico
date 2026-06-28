#include "adapter_ctx.h"

// Packed controller slot: (idx << 8) | addr when a controller is tracked, or
// CONTROLLER_NONE (all-ones) as the "no controller" sentinel. Packing both bytes into
// one 32-bit word lets core1 publish a replug with a single atomic store, so core0
// never observes a half-updated idx/addr pair (torn read).
#define CONTROLLER_NONE UINT32_MAX

static volatile adapter_state_t s_state = STATE_NONE;
static volatile uint32_t s_controller = CONTROLLER_NONE;
// True once the mounted controller actually sends a packet (heartbeat/input). A
// controller that survived a warm reset can mount but stay a "zombie" -- enumerated
// yet silent (no heartbeat, LED off). This is the real liveness signal, not mount.
static volatile bool s_alive = false;
// True once any controller has mounted this boot. Distinguishes a zombie (mounted but
// silent -> worth a recovery reboot) from no controller plugged in at all (don't
// reboot -- a controller is only needed to establish auth, not to keep running).
static volatile bool s_seen = false;
// Set when a running controller re-announces (replug that reset its GIP state). The
// host must re-init it or it announces forever and never streams input; serviced
// (debounced) by the core1 loop, which can safely block on the init tx.
static volatile bool s_reinit_pending = false;

void adapter_ctx_init(void) {
    s_state = STATE_NONE;
    s_controller = CONTROLLER_NONE;
    s_alive = false;
    s_seen = false;
    s_reinit_pending = false;
}

adapter_state_t adapter_get_state(void) { return s_state; }
void adapter_set_state(adapter_state_t s) { s_state = s; }

bool adapter_get_controller(uint8_t *idx, uint8_t *addr) {
    uint32_t packed = s_controller;  // single atomic load
    if (packed == CONTROLLER_NONE) return false;
    if (idx) *idx = (uint8_t)(packed >> 8);
    if (addr) *addr = (uint8_t)(packed & 0xFF);
    return true;
}

void adapter_set_controller(uint8_t idx, uint8_t addr) {
    s_controller = ((uint32_t)idx << 8) | (uint32_t)addr;  // single atomic store
}

void adapter_clear_controller(uint8_t idx) {
    uint32_t packed = s_controller;
    if (packed == CONTROLLER_NONE) return;
    if ((uint8_t)(packed >> 8) == idx) s_controller = CONTROLLER_NONE;
}

uint8_t adapter_get_controller_idx(void) {
    uint32_t packed = s_controller;
    if (packed == CONTROLLER_NONE) return UINT8_MAX;
    return (uint8_t)(packed >> 8);
}

bool adapter_controller_alive(void) { return s_alive; }
void adapter_set_controller_alive(bool v) { s_alive = v; }
bool adapter_controller_seen(void) { return s_seen; }
void adapter_set_controller_seen(bool v) { s_seen = v; }

void adapter_request_reinit(void) { s_reinit_pending = true; }

bool adapter_take_reinit(void) {
    bool prior = s_reinit_pending;
    s_reinit_pending = false;
    return prior;
}
