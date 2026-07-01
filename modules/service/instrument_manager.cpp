/*
 * Connected-instrument tracking + the Xbox add/drop-player notifications, as a modern-C++
 * service (orb::service::InstrumentManager) behind a small free-function API its C++
 * consumers (drums.cpp, guitar.cpp, midi.cpp, main.cpp) call. Same service-rewrite pattern
 * as adapter_ctx.cpp: a C++ object owns the state/logic, thin free functions forward into
 * the single instance.
 *
 * Concurrency (single-owner design): connect/disconnect are called from BOTH cores -- DRUMS
 * from core1 (the USB-MIDI mount/umount callbacks in drums.cpp, inside the core1 host task)
 * AND from core0 (serial-MIDI in midi.cpp + the timer-daemon disconnect); the guitars from
 * core1 only. Rather than mutate shared state under a cross-core lock on whichever core
 * calls in, connect_instrument()/disconnect_instrument() just POST a tiny {instrument,
 * connect} event onto an OSAL queue (non-blocking) and return. A SINGLE core0 owner task
 * (instrument_task -> instrument_manager_service) drains the queue and is the ONLY mutator
 * of connected_[] and the only place the add/drop packet is built + written to the device
 * fifo. This is the fix for a whole-processor lockup: the old path ran on core1 and, after
 * the flag flip, called xbox_fifo_write() which takes a FreeRTOS mutex with an infinite
 * (portMAX_DELAY) timeout -- blocking the timing-critical PIO-USB host core inside a USB
 * umount callback. Producers now never lock and never block, so core1 is never held off.
 *
 * Because the owner task is the sole writer, the flag check-then-set needs NO critical
 * section (the previous cross-core ScopedCritical is gone). connected_[] stays
 * std::atomic<bool> with relaxed load/store only (M0+ has no LDREX/STREX): that gives
 * tear-free single reads to the lock-free notify_* readers on the core0 device stack. A
 * constexpr constructor keeps the static instance constant-initialised -> BSS, no global
 * ctor.
 */
#include "core/section.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>  // std::to_underlying

#include "adapter.h"
#include "adapter_ctx.h"
#include "orb_debug.h"
#include "orb_enum.hpp"
#include "orb_log.h"

// instrument_manager.h (which pulls in xbox_one_protocol.h), packet_queue.h and
// xbox_one_protocol.h are all C++ headers now -- their functions (init_packet,
// xbox_fifo_write, the connect/disconnect/notify API) are plain C++ free functions
// defined in C++ TUs, so include them normally.
#include "instrument_manager.h"
#include "osal/queue.hpp"
#include "packet_queue.h"
#include "xbox_one_protocol.h"

namespace orb::service {
namespace {

constexpr std::size_t kInstrumentCount = std::to_underlying(N_INSTRUMENTS);

// Enum value -> contiguous array index (FIRST_INSTRUMENT == 0).
constexpr std::size_t idx(instruments_e instrument) {
    return std::to_underlying(instrument);
}

#if OPENRB_DEBUG_ENABLED
// Instrument name for trace/info logs -- replaces the old hand-rolled kInstrumentNames
// table with magic_enum (orb::enum_name), EXCEPT for value 0: GUITAR_ONE aliases
// FIRST_INSTRUMENT, which is declared first, and magic_enum returns the first-declared
// enumerator sharing a value -- i.e. "FIRST_INSTRUMENT". Map value 0 explicitly back to
// "GUITAR_ONE" so the log text stays byte-identical to the old table; GUITAR_TWO / DRUMS
// resolve directly. (.data() is a valid C string: magic_enum's names are null-terminated.)
inline const char *instrument_name(instruments_e instrument) {
    return instrument == GUITAR_ONE ? "GUITAR_ONE" : orb::enum_name(instrument).data();
}
#endif

// Add-player ("connect") and drop-player ("disconnect") wire payloads, one row per
// instrument, kept in flash (__in_flash) and byte-identical to the original C tables. Both
// tables are [N_INSTRUMENTS][22]: every notification copies the full 22-byte row (the
// drop-out rows are zero-padded past their 7 meaningful bytes), matching the original
// 22 entries for both connect and disconnect.
const std::uint8_t ORB_FLASH instrument_notify[kInstrumentCount][22] = {
    {0x22, 0x00, 0x00, 0x12, 0x00, 0x01, 0x14, 0x30, 0x00, 0x87, 0x67,
     0x00, 0x75, 0x00, 0x69, 0x00, 0x74, 0x00, 0x61, 0x00, 0x72, 0x00},
    {0x22, 0x00, 0x00, 0x12, 0x01, 0x01, 0x14, 0x30, 0x00, 0x87, 0x67,
     0x00, 0x75, 0x00, 0x69, 0x00, 0x74, 0x00, 0x61, 0x00, 0x72, 0x00},
    {0x22, 0x00, 0x00, 0x12, 0x02, 0x01, 0x1b, 0xad, 0x00, 0x88, 0x64,
     0x00, 0x72, 0x00, 0x75, 0x00, 0x6D, 0x00, 0x73, 0x00, 0x00, 0x00}};

const std::uint8_t ORB_FLASH instrument_drop_out[kInstrumentCount][22] = {
    {0x23, 0x00, 0x00, 0x01, 0x00, 0xFF, 0x05},
    {0x23, 0x00, 0x00, 0x01, 0x01, 0xFF, 0x05},
    {0x23, 0x00, 0x00, 0x01, 0x02, 0xFF, 0x05},
};

class InstrumentManager {
   public:
    void notify_all(xbox_packet_t *scratch) {
        LOG_DBG(CAT_DEV, "notify_xbox_of_all_instruments");
        for (instruments_e instrument : kAllInstruments) {
            if (!connected_[idx(instrument)].load(kRlx)) continue;
            build_packet(scratch, instrument, /*connect=*/true);
            xbox_fifo_write(scratch);
        }
    }

    void notify_single(instruments_e instrument, xbox_packet_t *scratch) {
        if (instrument < N_INSTRUMENTS) {
            LOG_DBG(CAT_DEV, "notify_xbox_of_single_instrument: %d - %s",
                    std::to_underlying(instrument), instrument_name(instrument));
            build_packet(scratch, instrument, /*connect=*/true);
            xbox_fifo_write(scratch);
        } else {
            LOG_DBG(CAT_DEV, "notify_xbox_of_single_instrument: %d", std::to_underlying(instrument));
        }
    }

    // Apply a hot-plug transition. Called ONLY by the owner task (core0), so the flag
    // check-then-set is single-writer and needs no lock. Builds the add/drop packet into
    // the manager's own scratch (owner-task-exclusive) and writes it to the device fifo.
    //
    // Ordering note: because the add/drop packet is now enqueued here (core0, deferred) while
    // instrument INPUT reports still go straight to the device fifo from core1, the old
    // happens-before ("add player" strictly precedes that player's first input, both on
    // core1) no longer holds. In STATE_RUNNING the console can briefly see an input packet
    // for an instrument it has not yet been told is connected. This is tolerated: the first
    // HID report is a USB round-trip away and the owner task drains the connect first in
    // practice; the console ignores input for an unannounced player. If it ever proves
    // observable, gate input forwarding on an owner-set "announced" flag instead.
    void apply(instruments_e instrument, bool connect) {
        if (!claim(instrument, connect)) return;  // no state change -> nothing to notify
        LOG_INFO(CAT_DEV, "%s %s!", instrument_name(instrument),
                 connect ? "connected" : "disconnected");

        if (orb::service::adapter().state() != adapter_state_t::STATE_RUNNING) return;

        build_packet(&scratch_, instrument, connect);
        xbox_fifo_write(&scratch_);
    }

   private:
    static constexpr std::memory_order kRlx = std::memory_order_relaxed;
    static constexpr std::array<instruments_e, kInstrumentCount> kAllInstruments{
        GUITAR_ONE, GUITAR_TWO, DRUMS};

    // Transition connected_[instrument] to `want`, returning true iff THIS call performed the
    // transition (i.e. the flag was not already `want`). No lock: the owner task is the sole
    // writer, so the load-then-store pair can't race another writer. Relaxed atomics keep the
    // notify_* readers on the core0 device stack tear-free.
    bool claim(instruments_e instrument, bool want) {
        if (connected_[idx(instrument)].load(kRlx) == want) return false;
        connected_[idx(instrument)].store(want, kRlx);
        return true;
    }

    // Copy an instrument's full wire row into the scratch packet and stamp its length.
    static void build_packet(xbox_packet_t *pkt, instruments_e instrument, bool connect) {
        const std::span<const std::uint8_t> src =
            connect ? std::span<const std::uint8_t>{instrument_notify[idx(instrument)]}
                    : std::span<const std::uint8_t>{instrument_drop_out[idx(instrument)]};
        std::ranges::copy(src, std::span<std::uint8_t>{pkt->buffer}.begin());
        init_packet(pkt, 0, static_cast<std::uint8_t>(src.size()));
    }

    std::array<std::atomic<bool>, kInstrumentCount> connected_{};  // all false -> BSS
    xbox_packet_t scratch_{};  // owner-task-exclusive add/drop-packet scratch
};

InstrumentManager g_instruments;

// One event per hot-plug edge. Trivially copyable so it moves through the OSAL queue by
// memcpy. Depth 8 comfortably covers 3 instruments churning; a full queue drops the event
// (logged) rather than ever blocking a producer -- the alternative that wedged core1.
struct InstrumentEvent {
    instruments_e instrument;
    bool connect;
};
orb::osal::Queue<InstrumentEvent, 8> g_events;

}  // namespace
}  // namespace orb::service

// --- public API (plain C++ free functions, declared in instrument_manager.h) ------------
// notify_* forward straight into the manager (core0 readers). connect/disconnect are the
// driver-facing producers: they only enqueue an event, so they never lock/block and are
// safe on core1's USB umount path. The single owner task applies them.

void notify_xbox_of_all_instruments(xbox_packet_t *scratch_space) {
    orb::service::g_instruments.notify_all(scratch_space);
}

void notify_xbox_of_single_instrument(instruments_e instrument, xbox_packet_t *scratch_space) {
    orb::service::g_instruments.notify_single(instrument, scratch_space);
}

void connect_instrument(instruments_e instrument) {
    if (!orb::service::g_events.send({instrument, /*connect=*/true}))
        LOG_WARN(CAT_DEV, "instrument event queue full; dropped connect %d",
                 std::to_underlying(instrument));
}

void disconnect_instrument(instruments_e instrument) {
    if (!orb::service::g_events.send({instrument, /*connect=*/false}))
        LOG_WARN(CAT_DEV, "instrument event queue full; dropped disconnect %d",
                 std::to_underlying(instrument));
}

void instrument_manager_init() { orb::service::g_events.create(); }

void instrument_manager_service() {
    orb::service::InstrumentEvent ev;
    if (orb::service::g_events.recv_blocking(ev))  // park until a hot-plug event arrives
        orb::service::g_instruments.apply(ev.instrument, ev.connect);
}
