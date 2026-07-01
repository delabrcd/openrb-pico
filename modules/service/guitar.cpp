/*
 * USB HID guitar input driver, as modern-C++ classes (orb::driver::Guitar,
 * orb::service::GuitarHost) behind the unchanged extern "C" TinyUSB host callbacks
 * (tuh_hid_*). Same seam pattern as the other rewrites: TinyUSB calls IN through fixed
 * C-linkage symbols, each of which is a thin shim that forwards through the app-layer
 * bridge (system.cpp) into the C++ state owned by orb::app::System. A self-contained
 * HID-input driver: parse a guitar HID report and emit a byte-identical Xbox input packet
 * via the injected DeviceTxFifo, plus the connect/disconnect-instrument notifications and
 * the two-guitar limit.
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
#include <span>
#include <utility>  // std::to_underlying

// clang-format off
#include "tusb.h" // IWYU pragma: export
#include "class/hid/hid_host.h"
// clang-format on
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

Guitar::Guitar(instruments_e player, orb::driver::DeviceTxFifo<xbox_packet_t, 16>& txfifo,
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

GuitarHost::GuitarHost(orb::driver::DeviceTxFifo<xbox_packet_t, 16>& txfifo,
                       orb::service::InstrumentManager& instruments)
    : guitars_{orb::driver::Guitar{GUITAR_ONE, txfifo, instruments},
               orb::driver::Guitar{GUITAR_TWO, txfifo, instruments}} {}

orb::driver::Guitar* GuitarHost::find_guitar(std::uint8_t dev_addr) {
    for (orb::driver::Guitar& g : guitars_)
        if (g.dev_addr() == dev_addr) return &g;
    return nullptr;
}

orb::driver::Guitar* GuitarHost::first_free_slot() {
    for (orb::driver::Guitar& g : guitars_)
        if (g.dev_addr() == 0) return &g;
    return nullptr;
}

void GuitarHost::mount(std::uint8_t dev_addr, std::uint8_t instance) {
    std::uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    LOG_INFO(CAT_DRUM, "HID device address = %d, instance = %d is mounted", dev_addr, instance);
    LOG_DBG(CAT_DRUM, "VID = %04x, PID = %04x", vid, pid);

    if (!orb::driver::is_supported_guitar(vid, pid)) return;

    LOG_INFO(CAT_DRUM, "GUITAR is in supported list");

    orb::driver::Guitar* slot = first_free_slot();
    if (slot == nullptr) {
        LOG_WARN(CAT_DRUM, "Already have 2 guitars connected, can't add another...");
        return;
    }
    slot->connect(dev_addr);

    // we need to request the first report
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        LOG_ERR(CAT_DRUM, "Error: cannot request to receive report");
    }
}

void GuitarHost::umount(std::uint8_t dev_addr) {
    if (orb::driver::Guitar* g = find_guitar(dev_addr)) g->disconnect();
}

void GuitarHost::report_received(std::uint8_t dev_addr, std::uint8_t instance,
                                 std::span<const std::uint8_t> report) {
    LOG_TRC(CAT_WIRE, "Report Received");
    LOG_HEXDUMP(CAT_WIRE, LOG_LEVEL_TRACE, report.data(), report.size());

    orb::driver::Guitar* g = find_guitar(dev_addr);
    if (g == nullptr) return;

    g->on_report(report);

    // continue to request to receive report
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        LOG_ERR(CAT_DRUM, "Error: cannot request to receive report");
    }
}

}  // namespace orb::service

// --- extern "C" TinyUSB host seam -------------------------------------------------------
// TinyUSB calls these by C symbol; each is a thin shim forwarding through the app-layer
// bridge (system.cpp), which reaches the single GuitarHost instance owned by
// orb::app::System -- service/ TUs never include app/ headers, so this file cannot reach
// System directly.

extern "C" void tuh_hid_mount_cb(std::uint8_t dev_addr, std::uint8_t instance,
                                 std::uint8_t const* desc_report, std::uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;
    guitar_on_hid_mount(dev_addr, instance);
}

extern "C" void tuh_hid_umount_cb(std::uint8_t dev_addr, std::uint8_t idx) {
    (void)idx;
    guitar_on_hid_umount(dev_addr);
}

extern "C" void tuh_hid_report_received_cb(std::uint8_t dev_addr, std::uint8_t instance,
                                           std::uint8_t const* report, std::uint16_t len) {
    guitar_on_hid_report(dev_addr, instance, {report, len});
}
