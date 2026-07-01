#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>  // std::to_underlying

#include "adapter_ctx.h"
#include "osal/queue.hpp"
#include "packet_queue.h"
#include "xbox_one_protocol.h"

// Instrument identity. The enumerators are used bare throughout (DRUMS, GUITAR_ONE, etc.)
// so they are imported to global scope below. FIRST_INSTRUMENT == 0 so the values double
// as contiguous array indices; the N_INSTRUMENTS sentinel is the count.
enum class instruments_e : std::uint8_t {
    FIRST_INSTRUMENT,
    GUITAR_ONE = FIRST_INSTRUMENT,
    GUITAR_TWO,
    DRUMS,
    N_INSTRUMENTS,
};

// The enumerators are used bare throughout the instrument API (DRUMS, GUITAR_ONE, ...),
// so import them to global scope; call sites keep their spelling. Conversions to an
// index / player-id byte / printf arg use std::to_underlying at the boundary.
using enum instruments_e;

namespace orb::service {

// One event per hot-plug edge. Trivially copyable so it moves through the OSAL queue by
// memcpy. Depth 8 comfortably covers 3 instruments churning; a full queue drops the event
// (logged) rather than ever blocking a producer -- the alternative that wedged core1.
struct InstrumentEvent {
    instruments_e instrument;
    bool connect;
};

// Connected-instrument tracking + the Xbox add/drop-player notifications.
//
// Concurrency (single-owner design): connect/disconnect are called from BOTH cores -- DRUMS
// from core1 (the USB-MIDI mount/umount callbacks in drums.cpp, inside the core1 host task)
// AND from core0 (serial-MIDI in midi.cpp + the timer-daemon disconnect); the guitars from
// core1 only. Rather than mutate shared state under a cross-core lock on whichever core
// calls in, post_connect()/post_disconnect() just POST a tiny {instrument, connect} event
// onto an OSAL queue (non-blocking) and return. A SINGLE core0 owner task (instrument_task
// -> service_once) drains the queue and is the ONLY mutator of connected_[] and the only
// place the add/drop packet is built + written to the device fifo. This is the fix for a
// whole-processor lockup: the old path ran on core1 and, after the flag flip, called
// xbox_fifo_write() which takes a FreeRTOS mutex with an infinite (indefinite) timeout --
// blocking the timing-critical PIO-USB host core inside a USB umount callback. Producers
// now never lock and never block, so core1 is never held off.
//
// Because the owner task is the sole writer, the flag check-then-set needs NO critical
// section (the previous cross-core ScopedCritical is gone). connected_[] stays
// std::atomic<bool> with relaxed load/store only (M0+ has no LDREX/STREX): that gives
// tear-free single reads to the lock-free notify_* readers on the core0 device stack.
class InstrumentManager {
   public:
    InstrumentManager(orb::service::AdapterState& adapter,
                       orb::driver::DeviceTxFifo<XboxPacket, 16>& txfifo,
                       orb::osal::Queue<InstrumentEvent, 8>& events)
        : adapter_(adapter), txfifo_(txfifo), events_(events) {}

    void notify_all(XboxPacket& scratch);
    void notify_single(instruments_e instrument, XboxPacket& scratch);

    // Apply a hot-plug transition. Called ONLY by the owner task (core0), so the flag
    // check-then-set is single-writer and needs no lock. Builds the add/drop packet into
    // the manager's own scratch (owner-task-exclusive) and writes it to the device fifo.
    void apply(instruments_e instrument, bool connect);

    // Driver-facing hot-plug API. post_connect/post_disconnect POST an event to the
    // instrument owner task and return immediately -- they NEVER take a lock or block, so
    // they are safe to call from the core1 USB-host mount/umount callbacks (the previous
    // synchronous path took a FreeRTOS mutex on core1 and could wedge the PIO-USB host).
    void post_connect(instruments_e instrument);
    void post_disconnect(instruments_e instrument);

    // Owner-task plumbing. init_queue() creates the event queue (call once before the
    // scheduler starts). service_once() blocks for one event and applies it; the core0
    // instrument_task loops on it.
    void init_queue();
    void service_once();

   private:
    static constexpr std::size_t kInstrumentCount = std::to_underlying(N_INSTRUMENTS);
    static constexpr std::memory_order kRlx = std::memory_order_relaxed;

    static constexpr std::size_t idx(instruments_e instrument) {
        return std::to_underlying(instrument);
    }

    // Transition connected_[instrument] to `want`, returning true iff THIS call performed the
    // transition (i.e. the flag was not already `want`). No lock: the owner task is the sole
    // writer, so the load-then-store pair can't race another writer. Relaxed atomics keep the
    // notify_* readers on the core0 device stack tear-free.
    bool claim(instruments_e instrument, bool want);

    // Copy an instrument's full wire row into the scratch packet and stamp its length.
    static void build_packet(XboxPacket& pkt, instruments_e instrument, bool connect);

    orb::service::AdapterState& adapter_;
    orb::driver::DeviceTxFifo<XboxPacket, 16>& txfifo_;
    orb::osal::Queue<InstrumentEvent, 8>& events_;

    std::array<std::atomic<bool>, kInstrumentCount> connected_{};  // all false -> BSS
    XboxPacket scratch_{};  // owner-task-exclusive add/drop-packet scratch
};

}  // namespace orb::service

// --- public API (plain C++ free functions); defined in instrument_manager.cpp -------------
//
// Re-announce the currently-connected instruments to the console. These only READ the
// connection state and build into the CALLER's scratch, so they stay callable directly
// from the core0 device stack (announce / CMD_ANNOUNCE handling).
void notify_xbox_of_all_instruments(XboxPacket& scratch_space);
void notify_xbox_of_single_instrument(instruments_e instrument, XboxPacket& scratch_space);

// Driver-facing hot-plug API. connect/disconnect_instrument POST an event to the
// instrument owner task and return immediately -- they NEVER take a lock or block, so they
// are safe to call from the core1 USB-host mount/umount callbacks (the previous
// synchronous path took a FreeRTOS mutex on core1 and could wedge the PIO-USB host). The
// single owner task (instrument_task, core0) is the sole mutator of the connection state
// and the only place the add/drop packet is built + queued to the device.
void connect_instrument(instruments_e instrument);
void disconnect_instrument(instruments_e instrument);

// Owner-task plumbing. instrument_manager_init() creates the event queue (call once before
// the scheduler starts). instrument_manager_service() blocks for one event and applies it;
// the core0 instrument_task loops on it.
void instrument_manager_init();
void instrument_manager_service();
