/*
 * Connected-instrument tracking + the Xbox add/drop-player notifications, as a modern-C++
 * service (orb::service::InstrumentManager) behind the unchanged extern "C" API its C
 * consumers (drums.c, guitar.c, midi.cpp, main.c) call. Same service-rewrite pattern as
 * adapter_ctx.cpp: a C++ object owns the state/logic, a thin C-linkage facade preserves
 * the call sites.
 *
 * Concurrency: every entry point (connect/disconnect from the core0 instrument-input task
 * and the core0 timer-service disconnect callback; notify_* from the core0 device stack)
 * runs on core0 -- see FreeRTOSConfig.h (timer daemon pinned core0) and main.c. The
 * connected_ flags were `volatile uint8_t` before; they become std::atomic<bool> with
 * memory_order_relaxed, which on M0+ emits the same plain ldrb/strb (no barrier) and so
 * reproduces the prior volatile semantics exactly. We use ONLY load()/store() -- never an
 * atomic read-modify-write: M0+ has no LDREX/STREX so fetch_add or exchange would emit an
 * interrupt-masking libcall. connect_instrument's check-then-set therefore stays a load
 * then a store, byte-for-byte the original `if (flag) return; flag = 1;` -- not an atomic
 * RMW, which is correct because all mutators are core0 tasks (the original volatile was
 * equally non-atomic across preemption). std::atomic<bool> has a constexpr constructor, so
 * the static instance is constant-initialised -> BSS, no global ctor.
 */
#include <pico.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "adapter.h"
#include "adapter_ctx.h"
#include "orb_debug.h"
#include "orb_log.h"

// xbox_one_protocol.h / packet_queue.h are plain C headers with no C-linkage seam of their
// own (init_packet, xbox_fifo_write are defined in C TUs); wrap them -- and
// instrument_manager.h, which pulls xbox_one_protocol.h in -- in extern "C" so those
// symbols resolve to their C definitions. Same local-seam pattern midi.cpp uses. (My own
// facade decls inside instrument_manager.h are already extern "C" via ORB_C_BEGIN; the
// nesting is harmless.)
extern "C" {
#include "instrument_manager.h"
#include "packet_queue.h"
#include "xbox_one_protocol.h"
}

namespace orb::service {
namespace {

constexpr std::size_t kInstrumentCount = N_INSTRUMENTS;

// Enum value -> contiguous array index (FIRST_INSTRUMENT == 0).
constexpr std::size_t idx(instruments_e instrument) {
    return static_cast<std::size_t>(instrument);
}

#if OPENRB_DEBUG_ENABLED
constexpr std::array<const char *, kInstrumentCount> kInstrumentNames{"GUITAR_ONE", "GUITAR_TWO",
                                                                      "DRUMS"};
#endif

// Add-player ("connect") and drop-player ("disconnect") wire payloads, one row per
// instrument, kept in flash (__in_flash) and byte-identical to the original C tables. Both
// tables are [N_INSTRUMENTS][22]: every notification copies the full 22-byte row (the
// drop-out rows are zero-padded past their 7 meaningful bytes), matching the original
// UTIL_NUM(row) == 22 length for both connect and disconnect.
const std::uint8_t __in_flash() instrument_notify[kInstrumentCount][22] = {
    {0x22, 0x00, 0x00, 0x12, 0x00, 0x01, 0x14, 0x30, 0x00, 0x87, 0x67,
     0x00, 0x75, 0x00, 0x69, 0x00, 0x74, 0x00, 0x61, 0x00, 0x72, 0x00},
    {0x22, 0x00, 0x00, 0x12, 0x01, 0x01, 0x14, 0x30, 0x00, 0x87, 0x67,
     0x00, 0x75, 0x00, 0x69, 0x00, 0x74, 0x00, 0x61, 0x00, 0x72, 0x00},
    {0x22, 0x00, 0x00, 0x12, 0x02, 0x01, 0x1b, 0xad, 0x00, 0x88, 0x64,
     0x00, 0x72, 0x00, 0x75, 0x00, 0x6D, 0x00, 0x73, 0x00, 0x00, 0x00}};

const std::uint8_t __in_flash() instrument_drop_out[kInstrumentCount][22] = {
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
            LOG_DBG(CAT_DEV, "notify_xbox_of_single_instrument: %d - %s", instrument,
                    kInstrumentNames[idx(instrument)]);
            build_packet(scratch, instrument, /*connect=*/true);
            xbox_fifo_write(scratch);
        } else {
            LOG_DBG(CAT_DEV, "notify_xbox_of_single_instrument: %d", instrument);
        }
    }

    void connect(instruments_e instrument, xbox_packet_t *scratch) {
        if (connected_[idx(instrument)].load(kRlx)) return;  // check ...
        LOG_INFO(CAT_DEV, "%s connected!", kInstrumentNames[idx(instrument)]);
        connected_[idx(instrument)].store(true, kRlx);  // ... then set (not an atomic RMW)

        if (orb::service::adapter().state() != STATE_RUNNING) return;

        build_packet(scratch, instrument, /*connect=*/true);
        xbox_fifo_write(scratch);
    }

    void disconnect(instruments_e instrument, xbox_packet_t *scratch) {
        if (!connected_[idx(instrument)].load(kRlx)) return;
        LOG_INFO(CAT_DEV, "%s disconnected!", kInstrumentNames[idx(instrument)]);
        connected_[idx(instrument)].store(false, kRlx);

        if (orb::service::adapter().state() != STATE_RUNNING) return;

        build_packet(scratch, instrument, /*connect=*/false);
        xbox_fifo_write(scratch);
    }

   private:
    static constexpr std::memory_order kRlx = std::memory_order_relaxed;
    static constexpr std::array<instruments_e, kInstrumentCount> kAllInstruments{
        GUITAR_ONE, GUITAR_TWO, DRUMS};

    // Copy an instrument's full wire row into the scratch packet and stamp its length.
    static void build_packet(xbox_packet_t *pkt, instruments_e instrument, bool connect) {
        const std::span<const std::uint8_t> src =
            connect ? std::span<const std::uint8_t>{instrument_notify[idx(instrument)]}
                    : std::span<const std::uint8_t>{instrument_drop_out[idx(instrument)]};
        std::ranges::copy(src, std::span<std::uint8_t>{pkt->buffer}.begin());
        init_packet(pkt, 0, static_cast<std::uint8_t>(src.size()));
    }

    std::array<std::atomic<bool>, kInstrumentCount> connected_{};  // all false -> BSS
};

InstrumentManager g_instruments;

}  // namespace
}  // namespace orb::service

// --- extern "C" facade (declared extern "C" via the orb_c_api.h seam in the header) ------
// The C consumers call these unchanged; they forward into the single InstrumentManager.

void notify_xbox_of_all_instruments(xbox_packet_t *scratch_space) {
    orb::service::g_instruments.notify_all(scratch_space);
}

void notify_xbox_of_single_instrument(instruments_e instrument, xbox_packet_t *scratch_space) {
    orb::service::g_instruments.notify_single(instrument, scratch_space);
}

void connect_instrument(instruments_e instrument, xbox_packet_t *scratch_space) {
    orb::service::g_instruments.connect(instrument, scratch_space);
}

void disconnect_instrument(instruments_e instrument, xbox_packet_t *scratch_space) {
    orb::service::g_instruments.disconnect(instrument, scratch_space);
}
