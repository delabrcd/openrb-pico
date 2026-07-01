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

namespace orb::app {
namespace {
std::optional<System> g_system;
}

System& system() { return *g_system; }
void system_init() { g_system.emplace(); }

}  // namespace orb::app

// --- adapter_ctx.h forwarder ----------------------------------------------------------
orb::service::AdapterState& orb::service::adapter() { return orb::app::system().adapter(); }

// --- packet_queue.h forwarders --------------------------------------------------------
void xbox_fifo_init(void) { orb::app::system().tx_fifo().init(); }
uint32_t xbox_fifo_read(xbox_packet_t *buffer) { return orb::app::system().tx_fifo().read(*buffer); }
uint32_t xbox_fifo_peek(xbox_packet_t *buffer) { return orb::app::system().tx_fifo().peek(*buffer); }
void xbox_fifo_advance(void) { orb::app::system().tx_fifo().advance(); }
uint32_t xbox_fifo_write(const xbox_packet_t *buffer) {
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

bool host_tx_send(const xbox_packet_t *pkt) { return orb::app::system().host_tx().send(*pkt); }
bool host_tx_recv(xbox_packet_t *pkt) { return orb::app::system().host_tx().recv(*pkt); }

bool midi_note_send(const midi_note_t *n) { return orb::app::system().midi_notes().send(*n); }
bool midi_note_recv(midi_note_t *n) { return orb::app::system().midi_notes().recv(*n); }

// --- instrument_manager.h forwarders ----------------------------------------------------
void notify_xbox_of_all_instruments(xbox_packet_t *scratch_space) {
    orb::app::system().instruments().notify_all(*scratch_space);
}

void notify_xbox_of_single_instrument(instruments_e instrument, xbox_packet_t *scratch_space) {
    orb::app::system().instruments().notify_single(instrument, *scratch_space);
}

void connect_instrument(instruments_e instrument) {
    orb::app::system().instruments().post_connect(instrument);
}

void disconnect_instrument(instruments_e instrument) {
    orb::app::system().instruments().post_disconnect(instrument);
}

void instrument_manager_init() { orb::app::system().instruments().init_queue(); }

void instrument_manager_service() { orb::app::system().instruments().service_once(); }
