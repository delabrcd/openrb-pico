/*
 * Composition-root bridge TU. Defines the single orb::app::System instance and every
 * free-function forwarder that reaches into it -- the ONLY place (besides main.cpp) allowed
 * to include system.hpp (service/ TUs never include app/ headers; this is where
 * app-reaching code lives per the layering rule).
 *
 * std::optional<System> gives System a trivial static-storage-duration slot (BSS, no
 * global ctor); system_init() emplaces it once, after set_sys_clock_khz (see main.cpp's
 * init()) so hardware-touching member ctors run post-clock.
 */
#include "system.hpp"

#include <optional>

#include "core/section.hpp"  // ORB_FAST -- preserved on the drum/serial-midi hot entry points

#include "drums_midi_seam.h"    // orb::driver::bind_drums_midi
#include "guitar_hid_driver.h"  // orb::driver::bind_guitar_hid

namespace orb::app {
namespace {
std::optional<System> g_system;
}

System& system() { return *g_system; }
void system_init() { g_system.emplace(); }

void bind_usb_seams() {
    orb::driver::bind_guitar_hid(system().guitars());
    orb::driver::bind_drums_midi(system().drums());
    orb::app::bind_host_controller(system().host_controller());
}

}  // namespace orb::app

// --- adapter_ctx.h forwarder ----------------------------------------------------------
orb::service::AdapterState& orb::service::adapter() { return orb::app::system().adapter(); }

// --- packet_queue.h forwarders --------------------------------------------------------
void xbox_fifo_init(void) { orb::app::system().tx_fifo().init(); }
uint32_t xbox_fifo_read(XboxPacket *buffer) { return orb::app::system().tx_fifo().read(*buffer); }
uint32_t xbox_fifo_peek(XboxPacket *buffer) { return orb::app::system().tx_fifo().peek(*buffer); }
void xbox_fifo_advance(void) { orb::app::system().tx_fifo().advance(); }
uint32_t xbox_fifo_write(const XboxPacket *buffer) {
    return orb::app::system().tx_fifo().write(*buffer);
}
uint32_t xbox_fifo_count(void) { return orb::app::system().tx_fifo().count(); }
bool xbox_fifo_empty(void) { return orb::app::system().tx_fifo().empty(); }
bool xbox_fifo_full(void) { return orb::app::system().tx_fifo().full(); }
void xbox_fifo_clear(void) { orb::app::system().tx_fifo().clear(); }

// --- app_queues.h forwarders -----------------------------------------------------------
void app_queues_init(void) {
    orb::app::system().host_tx().create();
    orb::app::system().midi_notes().create();
}

bool host_tx_send(const XboxPacket *pkt) { return orb::app::system().host_tx().send(*pkt); }
bool host_tx_recv(XboxPacket *pkt) { return orb::app::system().host_tx().recv(*pkt); }

// --- instrument_manager.h forwarders ----------------------------------------------------
void notify_xbox_of_all_instruments(XboxPacket& scratch_space) {
    orb::app::system().instruments().notify_all(scratch_space);
}

void notify_xbox_of_single_instrument(instruments_e instrument, XboxPacket& scratch_space) {
    orb::app::system().instruments().notify_single(instrument, scratch_space);
}

void connect_instrument(instruments_e instrument) {
    orb::app::system().instruments().post_connect(instrument);
}

void disconnect_instrument(instruments_e instrument) {
    orb::app::system().instruments().post_disconnect(instrument);
}

void instrument_manager_init() { orb::app::system().instruments().init_queue(); }

void instrument_manager_service() { orb::app::system().instruments().service_once(); }

// --- midi.h forwarders -------------------------------------------------------------------
void serial_midi_init() { orb::app::system().serial_midi().init(); }

// --- drums.h forwarders ------------------------------------------------------------------
void ORB_FAST(drum_task)() { orb::app::system().drums().tick(); }
