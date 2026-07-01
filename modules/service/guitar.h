#pragma once

// USB HID guitar input driver, as modern-C++ classes: orb::driver::Guitar (one connected
// slot) and orb::service::GuitarHost (the two-slot owner, reached by TinyUSB's tuh_hid_*_cb
// seam through the bridge forwarders declared at the bottom of this file). Declared here so
// orb::app::System can own a GuitarHost instance as a plain member; method bodies stay in
// guitar.cpp.

#include <array>
#include <cstdint>
#include <span>

#include "instrument_manager.h"  // orb::service::InstrumentManager, instruments_e
#include "packet_queue.h"        // orb::driver::DeviceTxFifo
#include "xbox_one_protocol.h"   // xbox_packet_t

namespace orb::driver {

// Per-slot guitar state. The player id (GUITAR_ONE/GUITAR_TWO) is the slot's array index in
// GuitarHost, passed in at construction; dev_addr_ == 0 means the slot is free.
class Guitar {
   public:
    Guitar(instruments_e player, orb::driver::DeviceTxFifo<xbox_packet_t, 16>& txfifo,
           orb::service::InstrumentManager& instruments);

    std::uint8_t dev_addr() const { return dev_addr_; }

    void connect(std::uint8_t dev_addr);
    void disconnect();
    void on_report(std::span<const std::uint8_t> report);

   private:
    orb::driver::DeviceTxFifo<xbox_packet_t, 16>& txfifo_;
    orb::service::InstrumentManager& instruments_;
    xbox_packet_t out_packet_{};
    std::uint8_t dev_addr_{0};
    instruments_e player_;
};

}  // namespace orb::driver

namespace orb::service {

// Owns the two guitar slots (GUITAR_ONE, GUITAR_TWO) and the TinyUSB HID mount/umount/report
// bookkeeping that used to live as free functions + file-static g_guitars in guitar.cpp.
class GuitarHost {
   public:
    GuitarHost(orb::driver::DeviceTxFifo<xbox_packet_t, 16>& txfifo,
              orb::service::InstrumentManager& instruments);

    void mount(std::uint8_t dev_addr, std::uint8_t instance);
    void umount(std::uint8_t dev_addr);
    void report_received(std::uint8_t dev_addr, std::uint8_t instance,
                         std::span<const std::uint8_t> report);

   private:
    orb::driver::Guitar* find_guitar(std::uint8_t dev_addr);
    orb::driver::Guitar* first_free_slot();

    std::array<orb::driver::Guitar, 2> guitars_;
};

}  // namespace orb::service

// Bridge forwarders for the TinyUSB host HID seam (defined in system.cpp -> forward to
// system().guitars().mount/umount/report_received). Declared here so guitar.cpp's
// extern "C" tuh_hid_*_cb callbacks can call them.
void guitar_on_hid_mount(std::uint8_t dev_addr, std::uint8_t instance);
void guitar_on_hid_umount(std::uint8_t dev_addr);
void guitar_on_hid_report(std::uint8_t dev_addr, std::uint8_t instance,
                          std::span<const std::uint8_t> report);
