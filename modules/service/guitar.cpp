/*
 * USB HID guitar input driver, as modern-C++ classes (orb::driver::Guitar,
 * orb::service::GuitarHost). No dependency on the vendored USB stack: the extern "C" TinyUSB
 * HID host callback seam and the vendor operations it used (fetching the VID/PID, requesting
 * the next report) live in modules/driver/guitar_hid_driver.cpp, which calls
 * GuitarHost::mount()/report_received() below and uses their bool return value to decide
 * whether to (re-)request the next report -- see that file for the seam. A self-contained
 * HID-input driver: parse a guitar HID report and emit a byte-identical Xbox input packet via
 * the injected DeviceTxFifo, plus the connect/disconnect-instrument notifications and the
 * two-guitar limit.
 *
 * Slot identity: GUITAR_ONE == 0 and GUITAR_TWO == 1 are contiguous, so the array index
 * doubles as the player id. Each Guitar owns its own out_packet scratch + the address of
 * the device bound to that slot (0 == free), exactly the prior guitar_1_data/guitar_2_data
 * file statics.
 */
#include "guitar.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>  // std::to_underlying

#include "orb_debug.h"
#include "orb_log.h"

// instrument_manager.h (which pulls in xbox_one_protocol.h), packet_queue.h and
// xbox_one_protocol.h are all C++ headers now -- their functions (fill_guitar_input_from_hid_report,
// the connect/disconnect API) are plain C++ free functions in C++ TUs -- so include them
// normally.
#include "instrument_manager.h"
#include "packet_queue.h"
#include "xbox_one_protocol.h"

namespace orb::driver {
namespace {

// Supported HID guitars, by USB (VID, PID). Replaces the VENDOR/PRODUCT switch macros with
// a data table so adding a device is a single row; lookup is a plain membership test.
struct VidPid {
    std::uint16_t vid;
    std::uint16_t pid;
};

constexpr std::array<VidPid, 1> kSupportedGuitars{{
    {0x1209, 0x2882},
}};

bool is_supported_guitar(std::uint16_t vid, std::uint16_t pid) {
    return std::ranges::any_of(kSupportedGuitars,
                               [&](VidPid d) { return d.vid == vid && d.pid == pid; });
}

}  // namespace

Guitar::Guitar(instruments_e player, orb::driver::DeviceTxFifo<XboxPacket, 16>& txfifo,
               orb::service::InstrumentManager& instruments)
    : txfifo_(txfifo), instruments_(instruments), player_(player) {}

void Guitar::connect(std::uint8_t dev_addr) {
    instruments_.post_connect(player_);  // posts an event; owner task (core0) does the notify
    dev_addr_ = dev_addr;
}

void Guitar::disconnect() {
    instruments_.post_disconnect(player_);  // non-blocking on core1's umount path
    dev_addr_ = 0;
}

void Guitar::on_report(std::span<const std::uint8_t> report) {
    fill_guitar_input_from_hid_report(report.data(), &out_packet_, std::to_underlying(player_));
    txfifo_.write(out_packet_);
}

}  // namespace orb::driver

namespace orb::service {

GuitarHost::GuitarHost(orb::driver::DeviceTxFifo<XboxPacket, 16>& txfifo,
                       orb::service::InstrumentManager& instruments)
    : guitars_{orb::driver::Guitar{GUITAR_ONE, txfifo, instruments},
               orb::driver::Guitar{GUITAR_TWO, txfifo, instruments}} {}

std::optional<std::reference_wrapper<orb::driver::Guitar>> GuitarHost::find_guitar(
    std::uint8_t dev_addr) {
    for (orb::driver::Guitar& g : guitars_)
        if (g.dev_addr() == dev_addr) return g;
    return std::nullopt;
}

std::optional<std::reference_wrapper<orb::driver::Guitar>> GuitarHost::first_free_slot() {
    for (orb::driver::Guitar& g : guitars_)
        if (g.dev_addr() == 0) return g;
    return std::nullopt;
}

bool GuitarHost::mount(std::uint8_t dev_addr, std::uint8_t instance, std::uint16_t vid,
                       std::uint16_t pid) {
    LOG_INFO(CAT_DRUM, "HID device address = %d, instance = %d is mounted", dev_addr, instance);
    LOG_DBG(CAT_DRUM, "VID = %04x, PID = %04x", vid, pid);

    if (!orb::driver::is_supported_guitar(vid, pid)) return false;

    LOG_INFO(CAT_DRUM, "GUITAR is in supported list");

    auto slot = first_free_slot();
    if (!slot) {
        LOG_WARN(CAT_DRUM, "Already have 2 guitars connected, can't add another...");
        return false;
    }
    slot->get().connect(dev_addr);
    return true;
}

void GuitarHost::umount(std::uint8_t dev_addr) {
    if (auto g = find_guitar(dev_addr)) g->get().disconnect();
}

bool GuitarHost::report_received(std::uint8_t dev_addr, std::span<const std::uint8_t> report) {
    LOG_TRC(CAT_WIRE, "Report Received");
    LOG_HEXDUMP(CAT_WIRE, LOG_LEVEL_TRACE, report.data(), report.size());

    auto g = find_guitar(dev_addr);
    if (!g) return false;

    g->get().on_report(report);
    return true;
}

}  // namespace orb::service
