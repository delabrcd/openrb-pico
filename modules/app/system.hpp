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
 * bridge TU -- the only place besides main.cpp allowed to include this header).
 *
 * P2: the three feature services -- SerialMidi (serial-MIDI parser), DrumEngine (drums), and
 * GuitarHost (the two guitar slots) -- join the same ownership + forwarder pattern. Further
 * modules land in P3+; at that point every remaining anonymous-namespace singleton and
 * free-function forwarder is retired. See the plan in docs (rearchitect/di-classes) and
 * AGENTS.md.
 */
#pragma once

#include "actuators.hpp"     // orb::board::Actuators
#include "adapter_ctx.h"     // orb::service::AdapterState
#include "app_queues.h"      // midi_note_t
#include "drums.h"           // orb::service::DrumEngine
#include "guitar.h"          // orb::service::GuitarHost
#include "instrument_manager.h"  // orb::service::InstrumentManager, InstrumentEvent
#include "midi.h"            // orb::service::SerialMidi
#include "osal/queue.hpp"    // orb::osal::Queue
#include "packet_queue.h"    // orb::driver::DeviceTxFifo
#include "xbox_one_protocol.h"  // XboxPacket

namespace orb::app {

class System {
   public:
    System()
        : instruments_(adapter_, tx_fifo_, instr_events_),
          serial_midi_(instruments_),
          drums_(adapter_, midi_note_q_, serial_midi_, tx_fifo_, instruments_),
          guitars_(tx_fifo_, instruments_) {}

    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // Accessors used by main's wiring, the app-layer bridge forwarders, and the seam binds.
    orb::service::AdapterState& adapter() { return adapter_; }
    orb::driver::DeviceTxFifo<XboxPacket, 16>& tx_fifo() { return tx_fifo_; }
    orb::osal::Queue<orb::service::InstrumentEvent, 8>& instr_events() { return instr_events_; }
    orb::osal::Queue<XboxPacket, 8>& host_tx() { return host_tx_q_; }
    orb::osal::Queue<midi_note_t, 32>& midi_notes() { return midi_note_q_; }
    orb::service::InstrumentManager& instruments() { return instruments_; }
    orb::service::SerialMidi& serial_midi() { return serial_midi_; }
    orb::service::DrumEngine& drums() { return drums_; }
    orb::service::GuitarHost& guitars() { return guitars_; }
    orb::board::Actuators& actuators() { return actuators_; }

   private:
    // Members (leaves -> services -> app/orchestration). Declaration order IS construction
    // order: adapter_/tx_fifo_/instr_events_ must exist before instruments_ is constructed
    // (it stores references to them); instruments_/tx_fifo_/midi_note_q_ must exist before
    // serial_midi_/drums_/guitars_ (they store references to those in turn), and serial_midi_
    // must precede drums_ (drums_ stores a reference to it).
    orb::board::Actuators actuators_;  // leaf (no injected deps); GPIO emplaced at runtime
    orb::service::AdapterState adapter_;
    orb::driver::DeviceTxFifo<XboxPacket, 16> tx_fifo_;
    orb::osal::Queue<orb::service::InstrumentEvent, 8> instr_events_;
    orb::osal::Queue<XboxPacket, 8> host_tx_q_;
    orb::osal::Queue<midi_note_t, 32> midi_note_q_;
    orb::service::InstrumentManager instruments_;
    orb::service::SerialMidi serial_midi_;
    orb::service::DrumEngine drums_;
    orb::service::GuitarHost guitars_;
};

// The single composition-root instance (defined in system.cpp).
System& system();
void system_init();

// Binds the driver-owned TinyUSB seams (modules/driver/guitar_hid_driver.h,
// modules/driver/drums_midi_seam.h) to this System's GuitarHost/DrumEngine instances. Must be
// called after system_init() and before tuh_init() runs (main.cpp's init(), right after
// orb::app::system_init()) -- see modules/core/seam_anchor.hpp for why the bind must happen
// before the vendor stack that fires the seam starts.
void bind_usb_seams();

}  // namespace orb::app
