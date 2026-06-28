/*
 * Cross-core adapter state, as a modern-C++ service (orb::service::AdapterState) behind
 * the unchanged extern "C" adapter_* API its C consumers (main.c, drums.c,
 * instrument_manager.c) call. This is the service-rewrite pattern for the rearchitecture:
 * C++ object holds the state, a thin C-linkage facade preserves the call sites until they
 * too become C++.
 *
 * Concurrency (RP2040 dual Cortex-M0+): aligned word/byte access is atomic and lock-free.
 * std::atomic load()/store() compile to a plain ldr/str here -- we use ONLY load/store
 * (never an atomic read-modify-write: M0+ has no LDREX/STREX, so fetch_add/exchange would
 * emit an interrupt-masking libcall). The fields are independent signals with no
 * cross-field happens-before requirement, so memory_order_relaxed reproduces exactly the
 * previous `volatile` semantics (plain accesses, no barrier). std::atomic has a constexpr
 * constructor, so the static instance is constant-initialised -> BSS, no global ctor.
 */
#include "adapter_ctx.h"

#include <atomic>
#include <cstdint>

namespace orb::service {

class AdapterState {
   public:
    // Packed controller slot: (idx << 8) | addr, or kNone as the "no controller" sentinel.
    // One 32-bit word so core1 publishes a replug with a single store and core0 never sees
    // a half-updated idx/addr pair.
    static constexpr uint32_t kNone = UINT32_MAX;

    void reset() {
        state_.store(STATE_NONE, kRlx);
        controller_.store(kNone, kRlx);
        alive_.store(false, kRlx);
        seen_.store(false, kRlx);
        reinit_.store(false, kRlx);
    }

    adapter_state_t state() const { return state_.load(kRlx); }
    void set_state(adapter_state_t s) { state_.store(s, kRlx); }

    bool controller(uint8_t* idx, uint8_t* addr) const {
        uint32_t packed = controller_.load(kRlx);  // single atomic load
        if (packed == kNone) return false;
        if (idx) *idx = static_cast<uint8_t>(packed >> 8);
        if (addr) *addr = static_cast<uint8_t>(packed & 0xFF);
        return true;
    }
    void set_controller(uint8_t idx, uint8_t addr) {
        controller_.store((static_cast<uint32_t>(idx) << 8) | addr, kRlx);  // single store
    }
    void clear_controller(uint8_t idx) {  // only clears if idx matches the tracked one
        uint32_t packed = controller_.load(kRlx);
        if (packed != kNone && static_cast<uint8_t>(packed >> 8) == idx)
            controller_.store(kNone, kRlx);
    }
    uint8_t controller_idx() const {
        uint32_t packed = controller_.load(kRlx);
        return packed == kNone ? UINT8_MAX : static_cast<uint8_t>(packed >> 8);
    }

    bool alive() const { return alive_.load(kRlx); }
    void set_alive(bool v) { alive_.store(v, kRlx); }
    bool seen() const { return seen_.load(kRlx); }
    void set_seen(bool v) { seen_.store(v, kRlx); }

    void request_reinit() { reinit_.store(true, kRlx); }
    bool take_reinit() {  // producer + consumer are both core1; plain load+store (not a RMW)
        bool prior = reinit_.load(kRlx);
        reinit_.store(false, kRlx);
        return prior;
    }

   private:
    static constexpr std::memory_order kRlx = std::memory_order_relaxed;

    std::atomic<adapter_state_t> state_{STATE_NONE};  // written core0, read both
    std::atomic<uint32_t> controller_{kNone};          // written core1, read both
    std::atomic<bool> alive_{false};                   // written core1, read core0
    std::atomic<bool> seen_{false};                    // written core1, read core0
    std::atomic<bool> reinit_{false};                  // core1 only
};

static AdapterState g_adapter;

}  // namespace orb::service

// --- extern "C" facade (declared extern "C" via the orb_c_api.h seam in adapter_ctx.h) --
// The C consumers call these unchanged; they forward into the single AdapterState object.

void adapter_ctx_init(void) { orb::service::g_adapter.reset(); }

adapter_state_t adapter_get_state(void) { return orb::service::g_adapter.state(); }
void adapter_set_state(adapter_state_t s) { orb::service::g_adapter.set_state(s); }

bool adapter_get_controller(uint8_t* idx, uint8_t* addr) {
    return orb::service::g_adapter.controller(idx, addr);
}
void adapter_set_controller(uint8_t idx, uint8_t addr) {
    orb::service::g_adapter.set_controller(idx, addr);
}
void adapter_clear_controller(uint8_t idx) { orb::service::g_adapter.clear_controller(idx); }
uint8_t adapter_get_controller_idx(void) { return orb::service::g_adapter.controller_idx(); }

bool adapter_controller_alive(void) { return orb::service::g_adapter.alive(); }
void adapter_set_controller_alive(bool v) { orb::service::g_adapter.set_alive(v); }
bool adapter_controller_seen(void) { return orb::service::g_adapter.seen(); }
void adapter_set_controller_seen(bool v) { orb::service::g_adapter.set_seen(v); }

void adapter_request_reinit(void) { orb::service::g_adapter.request_reinit(); }
bool adapter_take_reinit(void) { return orb::service::g_adapter.take_reinit(); }
