/*
 * orb::app::System — the composition root for the whole firmware.
 *
 * One object owns every module as a plain member (no heap, no pointers) and, in its
 * constructor, constructs them in dependency order and injects references — proper static
 * dependency injection. It is placed in `static std::optional<System>` in system.cpp and
 * `.emplace()`d inside orb::app::system_init() (called from main.cpp's init() right after
 * set_sys_clock_khz), so hardware-touching member ctors (Uart, GPIO) run post-clock. Ctors
 * only store references + POD setup; heavy bring-up (tud_init/tuh_init, hub reset, seam
 * binds) stays in the namespaced init()/run() paths.
 *
 * P1 (DI re-architecture): the leaf service objects (AdapterState, the device TX fifo, the
 * inter-task queues, InstrumentManager) are now owned here and reached, by every existing
 * caller, through the free-function forwarders defined in system.cpp (the composition-root
 * bridge TU -- the only place besides main.cpp allowed to include this header). Further
 * modules land in P2+; at that point every remaining anonymous-namespace singleton and
 * free-function forwarder is retired. See the plan in docs (rearchitect/di-classes) and
 * AGENTS.md.
 */
#pragma once

#include "adapter_ctx.h"     // orb::service::AdapterState
#include "app_queues.h"      // midi_note_t
#include "instrument_manager.h"  // orb::service::InstrumentManager, InstrumentEvent
#include "osal/queue.hpp"    // orb::osal::Queue
#include "packet_queue.h"    // orb::driver::DeviceTxFifo
#include "xbox_one_protocol.h"  // xbox_packet_t

namespace orb::app {

class System {
   public:
    System() : instruments_(adapter_, tx_fifo_, instr_events_) {}

    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // Accessors used by main's wiring, the app-layer bridge forwarders, and the seam binds.
    orb::service::AdapterState& adapter() { return adapter_; }
    orb::driver::DeviceTxFifo<xbox_packet_t, 16>& tx_fifo() { return tx_fifo_; }
    orb::osal::Queue<orb::service::InstrumentEvent, 8>& instr_events() { return instr_events_; }
    orb::osal::Queue<xbox_packet_t, 8>& host_tx() { return host_tx_q_; }
    orb::osal::Queue<midi_note_t, 32>& midi_notes() { return midi_note_q_; }
    orb::service::InstrumentManager& instruments() { return instruments_; }

   private:
    // Members (leaves -> services -> app/orchestration). Declaration order IS construction
    // order: adapter_/tx_fifo_/instr_events_ must exist before instruments_ is constructed
    // (it stores references to them).
    orb::service::AdapterState adapter_;
    orb::driver::DeviceTxFifo<xbox_packet_t, 16> tx_fifo_;
    orb::osal::Queue<orb::service::InstrumentEvent, 8> instr_events_;
    orb::osal::Queue<xbox_packet_t, 8> host_tx_q_;
    orb::osal::Queue<midi_note_t, 32> midi_note_q_;
    orb::service::InstrumentManager instruments_;
};

// The single composition-root instance (defined in system.cpp).
System& system();
void system_init();

}  // namespace orb::app
