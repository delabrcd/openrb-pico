/*
 * Connected-instrument tracking + the Xbox add/drop-player notifications, as a modern-C++
 * service (orb::service::InstrumentManager) behind a small free-function API its C++
 * consumers (drums.cpp, guitar.cpp, midi.cpp, main.cpp) call. Same service-rewrite pattern
 * as adapter_ctx.cpp: a C++ object owns the state/logic (now owned by orb::app::System, see
 * modules/app/system.hpp), thin free functions forward into it (definitions in
 * modules/app/system.cpp, the composition-root bridge).
 *
 * The class declaration + InstrumentEvent live in instrument_manager.h; this TU keeps the
 * out-of-line method bodies and the ORB_FLASH wire-payload tables. See instrument_manager.h
 * for the full concurrency rationale (single-owner core0 task, non-blocking producers).
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

// Local mirror of the private InstrumentManager::kInstrumentCount (same value), needed for
// the flash table extents below -- private class members aren't visible at namespace scope.
constexpr std::size_t kInstrumentCount = std::to_underlying(N_INSTRUMENTS);

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

constexpr std::array<instruments_e, kInstrumentCount> kAllInstruments{
    GUITAR_ONE, GUITAR_TWO, DRUMS};

}  // namespace

void InstrumentManager::notify_all(xbox_packet_t &scratch) {
    LOG_DBG(CAT_DEV, "notify_xbox_of_all_instruments");
    for (instruments_e instrument : kAllInstruments) {
        if (!connected_[idx(instrument)].load(kRlx)) continue;
        build_packet(scratch, instrument, /*connect=*/true);
        txfifo_.write(scratch);
    }
}

void InstrumentManager::notify_single(instruments_e instrument, xbox_packet_t &scratch) {
    if (instrument < N_INSTRUMENTS) {
        LOG_DBG(CAT_DEV, "notify_xbox_of_single_instrument: %d - %s",
                std::to_underlying(instrument), instrument_name(instrument));
        build_packet(scratch, instrument, /*connect=*/true);
        txfifo_.write(scratch);
    } else {
        LOG_DBG(CAT_DEV, "notify_xbox_of_single_instrument: %d", std::to_underlying(instrument));
    }
}

// Ordering note: because the add/drop packet is now enqueued here (core0, deferred) while
// instrument INPUT reports still go straight to the device fifo from core1, the old
// happens-before ("add player" strictly precedes that player's first input, both on
// core1) no longer holds. In STATE_RUNNING the console can briefly see an input packet
// for an instrument it has not yet been told is connected. This is tolerated: the first
// HID report is a USB round-trip away and the owner task drains the connect first in
// practice; the console ignores input for an unannounced player. If it ever proves
// observable, gate input forwarding on an owner-set "announced" flag instead.
void InstrumentManager::apply(instruments_e instrument, bool connect) {
    if (!claim(instrument, connect)) return;  // no state change -> nothing to notify
    LOG_INFO(CAT_DEV, "%s %s!", instrument_name(instrument), connect ? "connected" : "disconnected");

    if (adapter_.state() != adapter_state_t::STATE_RUNNING) return;

    build_packet(scratch_, instrument, connect);
    txfifo_.write(scratch_);
}

void InstrumentManager::post_connect(instruments_e instrument) {
    if (!events_.send({instrument, /*connect=*/true}))
        LOG_WARN(CAT_DEV, "instrument event queue full; dropped connect %d",
                 std::to_underlying(instrument));
}

void InstrumentManager::post_disconnect(instruments_e instrument) {
    if (!events_.send({instrument, /*connect=*/false}))
        LOG_WARN(CAT_DEV, "instrument event queue full; dropped disconnect %d",
                 std::to_underlying(instrument));
}

void InstrumentManager::init_queue() { events_.create(); }

void InstrumentManager::service_once() {
    InstrumentEvent ev;
    if (events_.recv_blocking(ev))  // park until a hot-plug event arrives
        apply(ev.instrument, ev.connect);
}

bool InstrumentManager::claim(instruments_e instrument, bool want) {
    if (connected_[idx(instrument)].load(kRlx) == want) return false;
    connected_[idx(instrument)].store(want, kRlx);
    return true;
}

void InstrumentManager::build_packet(xbox_packet_t &pkt, instruments_e instrument, bool connect) {
    const std::span<const std::uint8_t> src =
        connect ? std::span<const std::uint8_t>{instrument_notify[idx(instrument)]}
                : std::span<const std::uint8_t>{instrument_drop_out[idx(instrument)]};
    std::ranges::copy(src, std::span<std::uint8_t>{pkt.buffer}.begin());
    init_packet(&pkt, 0, static_cast<std::uint8_t>(src.size()));
}

}  // namespace orb::service
