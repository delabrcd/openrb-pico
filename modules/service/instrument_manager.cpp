/*
 * Connected-instrument tracking + the Xbox add/drop-player notifications, as a modern-C++
 * service (orb::service::InstrumentManager) behind a small free-function API its C++
 * consumers (drums.cpp, guitar.cpp, midi.cpp, main.cpp) call. Same service-rewrite pattern
 * as adapter_ctx.cpp: a C++ object owns the state/logic, thin free functions forward into
 * the single instance.
 *
 * Concurrency: connect/disconnect run on BOTH cores -- DRUMS connects/disconnects from core1
 * (the USB-MIDI mount/umount callbacks in drums.cpp, which run inside the core1 host task)
 * AND from core0 (serial-MIDI in midi.cpp + the timer-daemon disconnect, both core0); the
 * guitars connect/disconnect from core1 only. notify_* run on core0 (the device stack). So
 * connected_[DRUMS] is a genuinely cross-core flag. (The original C globals were `volatile
 * uint8_t` with the same cross-core callers -- the non-atomic check-then-set was a latent
 * race there too; this is the corrected version, not a regression.)
 *
 * The flags are std::atomic<bool>, relaxed load/store only (M0+ has no LDREX/STREX, so an
 * atomic RMW would emit an interrupt-masking libcall) -- which gives tear-free single reads
 * for the lock-free notify_* readers on core0. But a relaxed load THEN a relaxed store is
 * not atomic as a pair: two cores could both pass the "not connected" check and both
 * publish a connect. So connect()/disconnect() wrap ONLY the check-then-set of the flag in
 * an orb::osal::ScopedCritical (FreeRTOS SMP critical section -> spinlock + IRQ mask = true
 * cross-core mutual exclusion). The guarded body is two instructions; the build+FIFO-write
 * stays OUTSIDE the lock (each caller owns its scratch, and xbox_fifo_write is already
 * mutex-safe), so core1 is held off only at rare mount/umount events, never the stream path.
 * std::atomic<bool> has a constexpr constructor, so the static instance is constant-
 * initialised -> BSS, no global ctor.
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
#include "orb_enum.hpp"
#include "orb_log.h"

// instrument_manager.h (which pulls in xbox_one_protocol.h), packet_queue.h and
// xbox_one_protocol.h are all C++ headers now -- their functions (init_packet,
// xbox_fifo_write, the connect/disconnect/notify API) are plain C++ free functions
// defined in C++ TUs, so include them normally.
#include "instrument_manager.h"
#include "osal/critical.hpp"
#include "packet_queue.h"
#include "xbox_one_protocol.h"

namespace orb::service {
namespace {

constexpr std::size_t kInstrumentCount = N_INSTRUMENTS;

// Enum value -> contiguous array index (FIRST_INSTRUMENT == 0).
constexpr std::size_t idx(instruments_e instrument) {
    return static_cast<std::size_t>(instrument);
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
                    instrument_name(instrument));
            build_packet(scratch, instrument, /*connect=*/true);
            xbox_fifo_write(scratch);
        } else {
            LOG_DBG(CAT_DEV, "notify_xbox_of_single_instrument: %d", instrument);
        }
    }

    void connect(instruments_e instrument, xbox_packet_t *scratch) {
        if (!claim(instrument, /*want=*/true)) return;  // atomic not-connected -> connected
        LOG_INFO(CAT_DEV, "%s connected!", instrument_name(instrument));

        if (orb::service::adapter().state() != STATE_RUNNING) return;

        build_packet(scratch, instrument, /*connect=*/true);
        xbox_fifo_write(scratch);
    }

    void disconnect(instruments_e instrument, xbox_packet_t *scratch) {
        if (!claim(instrument, /*want=*/false)) return;  // atomic connected -> not-connected
        LOG_INFO(CAT_DEV, "%s disconnected!", instrument_name(instrument));

        if (orb::service::adapter().state() != STATE_RUNNING) return;

        build_packet(scratch, instrument, /*connect=*/false);
        xbox_fifo_write(scratch);
    }

   private:
    static constexpr std::memory_order kRlx = std::memory_order_relaxed;
    static constexpr std::array<instruments_e, kInstrumentCount> kAllInstruments{
        GUITAR_ONE, GUITAR_TWO, DRUMS};

    // Atomically transition connected_[instrument] to `want`, returning true iff THIS call
    // performed the transition (i.e. the flag was not already `want`). The load+store pair is
    // guarded by a cross-core critical section so two cores can't both win the same
    // transition (a relaxed load-then-store is not atomic as a pair on M0+).
    bool claim(instruments_e instrument, bool want) {
        orb::osal::ScopedCritical guard;
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
};

InstrumentManager g_instruments;

}  // namespace
}  // namespace orb::service

// --- public API (plain C++ free functions, declared in instrument_manager.h) ------------
// The consumers call these unchanged; they forward into the single InstrumentManager.

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
