/*
 * TinyUSB host MIDI seam for the drum service. The extern "C" tuh_midi_*_cb callbacks, the
 * tusb read call (tuh_midi_stream_read), and the connected-device address they all share used
 * to live in modules/service/drums.cpp / DrumEngine; they move here so modules/service/ stays
 * completely tusb-free (driver/ links service/ and is allowed to include tusb.h /
 * usb_midi_host.h directly -- see the P3 step of the static-DI re-architecture).
 *
 * s_midi_dev_addr is USB-host state: it is only ever read/written on core1 (the mount/umount
 * callbacks and the read_midi_host drain below run there), exactly like the
 * DrumEngine::midi_dev_addr_ member it replaces -- so it moves out of the service along with
 * the tusb calls that use it.
 *
 * The single orb::core::SeamAnchor<DrumEngine> is bound once, from
 * orb::app::bind_usb_seams(), before tuh_init() runs on core1.
 */
#include "drums_midi_seam.h"

#include <cstdint>

#include "usb_midi_host.h"

#include "core/section.hpp"      // ORB_FAST
#include "core/seam_anchor.hpp"
#include "app_queues.h"          // midi_note_t
#include "orb_log.h"

namespace orb::driver {
namespace {
orb::core::SeamAnchor<orb::service::DrumEngine> g_drums;
// Connected USB MIDI device address (0 == none connected). core1-only.
std::uint8_t s_midi_dev_addr = 0;
}  // namespace

void bind_drums_midi(orb::service::DrumEngine& engine) { g_drums.bind(engine); }

}  // namespace orb::driver

// --- core1: drain the USB-host MIDI FIFO ----------------------------------------------------
// Same core as tuh_midi_mount_cb (which sets s_midi_dev_addr). Drains regardless of adapter
// state so the FIFO can't overflow; hands each complete message to core0 via the queue.
void ORB_FAST(drums_read_midi_host)(void) {
    std::uint8_t cable_num;
    std::uint8_t msg[48];
    while (tuh_midi_stream_read(orb::driver::s_midi_dev_addr, &cable_num, msg, sizeof(msg)) !=
           0) {
        if (orb::driver::g_drums)
            orb::driver::g_drums->push_host_note(midi_note_t{{msg[0], msg[1], msg[2]}});
    }
}

// --- extern "C" TinyUSB host MIDI seam -------------------------------------------------------
// TinyUSB calls these by C symbol; each reaches the single DrumEngine instance (owned by
// orb::app::System) through the SeamAnchor bound at init.

extern "C" void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep,
                                  uint8_t num_cables_rx, uint16_t num_cables_tx) {
    LOG_INFO(CAT_DRUM,
             "MIDI device address = %u, IN endpoint %u has %u cables, OUT endpoint %u has %u "
             "cables",
             dev_addr, in_ep & 0xf, num_cables_rx, out_ep & 0xf, num_cables_tx);

    if (orb::driver::s_midi_dev_addr == 0) {
        // then no MIDI device is currently connected
        orb::driver::s_midi_dev_addr = dev_addr;
        if (orb::driver::g_drums) orb::driver::g_drums->on_midi_connected();
    } else {
        LOG_WARN(CAT_DRUM,
                 "A different USB MIDI Device is already connected. Only one device at a time "
                 "is supported in this program; device is disabled");
    }
}

// Invoked when device with hid interface is un-mounted
extern "C" void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance) {
    if (dev_addr == orb::driver::s_midi_dev_addr) {
        orb::driver::s_midi_dev_addr = 0;
        LOG_INFO(CAT_DRUM, "MIDI device address = %d, instance = %d is unmounted", dev_addr,
                 instance);
        if (orb::driver::g_drums) orb::driver::g_drums->on_midi_disconnected();
    } else {
        LOG_INFO(CAT_DRUM, "Unused MIDI device address = %d, instance = %d is unmounted",
                 dev_addr, instance);
    }
}
