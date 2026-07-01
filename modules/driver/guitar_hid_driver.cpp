/*
 * TinyUSB HID host seam for the guitar service. The extern "C" tuh_hid_*_cb callbacks and the
 * tusb operations they used (tuh_vid_pid_get, tuh_hid_receive_report) used to live in
 * modules/service/guitar.cpp; they move here so modules/service/ stays completely tusb-free
 * (driver/ links service/ and is allowed to include tusb.h directly -- see the P3 step of the
 * static-DI re-architecture). orb::service::GuitarHost::mount/report_received now return
 * whether the driver should (re-)request the next report, so the report-request timing is
 * unchanged even though the tusb call moved out of the service.
 *
 * The single orb::core::SeamAnchor<GuitarHost> is bound once, from
 * orb::app::bind_usb_seams(), before tuh_init() runs on core1.
 */
#include "guitar_hid_driver.h"

#include <cstdint>
#include <span>

// clang-format off
#include "tusb.h" // IWYU pragma: export
#include "class/hid/hid_host.h"
// clang-format on

#include "core/seam_anchor.hpp"
#include "orb_log.h"

namespace orb::driver {
namespace {
orb::core::SeamAnchor<orb::service::GuitarHost> g_guitar_hid;
}  // namespace

void bind_guitar_hid(orb::service::GuitarHost& host) { g_guitar_hid.bind(host); }

}  // namespace orb::driver

// --- extern "C" TinyUSB host HID seam -------------------------------------------------------
// TinyUSB calls these by C symbol; each reaches the single GuitarHost instance (owned by
// orb::app::System) through the SeamAnchor bound at init, then re-issues the tusb report
// request iff the service says the report advanced its state -- exactly the prior
// mount()/report_received() timing, just split across the driver/service boundary.

extern "C" void tuh_hid_mount_cb(std::uint8_t dev_addr, std::uint8_t instance,
                                 std::uint8_t const* desc_report, std::uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    std::uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    if (!orb::driver::g_guitar_hid) return;

    bool accepted = orb::driver::g_guitar_hid->mount(dev_addr, instance, vid, pid);
    if (accepted) {
        // we need to request the first report
        if (!tuh_hid_receive_report(dev_addr, instance)) {
            LOG_ERR(CAT_DRUM, "Error: cannot request to receive report");
        }
    }
}

extern "C" void tuh_hid_umount_cb(std::uint8_t dev_addr, std::uint8_t idx) {
    (void)idx;
    if (orb::driver::g_guitar_hid) orb::driver::g_guitar_hid->umount(dev_addr);
}

extern "C" void tuh_hid_report_received_cb(std::uint8_t dev_addr, std::uint8_t instance,
                                           std::uint8_t const* report, std::uint16_t len) {
    if (!orb::driver::g_guitar_hid) return;

    bool ok = orb::driver::g_guitar_hid->report_received(dev_addr, std::span{report, len});
    if (ok) {
        // continue to request to receive report
        if (!tuh_hid_receive_report(dev_addr, instance)) {
            LOG_ERR(CAT_DRUM, "Error: cannot request to receive report");
        }
    }
}
