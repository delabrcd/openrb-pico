#pragma once

// USB HID guitar input driver, as modern-C++ classes: orb::driver::Guitar (one connected
// slot) and orb::service::GuitarHost (the two-slot owner). GuitarHost has no dependency on
// the vendored USB stack -- the TinyUSB HID host callback seam and the vendor operations it
// used (fetching the VID/PID, requesting the next report) live in
// modules/driver/guitar_hid_driver.cpp, which calls mount()/report_received() below and uses
// their bool return to decide whether to (re-)request the next report. Declared here so
// orb::app::System can own a GuitarHost instance as a plain member; method bodies stay in
// guitar.cpp.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
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

// Owns the two guitar slots (GUITAR_ONE, GUITAR_TWO) and the HID mount/umount/report
// bookkeeping that used to live as free functions + file-static g_guitars in guitar.cpp.
// No dependency on the vendored USB stack: mount()/report_received() take the
// already-fetched VID/PID and report bytes and return whether the caller (the driver seam)
// should ask the USB stack for the next report -- they never touch it themselves.
class GuitarHost {
   public:
    GuitarHost(orb::driver::DeviceTxFifo<xbox_packet_t, 16>& txfifo,
              orb::service::InstrumentManager& instruments);

    // Returns true iff a supported guitar was assigned a free slot -- the driver seam should
    // request the first report iff this returns true.
    bool mount(std::uint8_t dev_addr, std::uint8_t instance, std::uint16_t vid,
              std::uint16_t pid);
    void umount(std::uint8_t dev_addr);
    // Returns true iff a connected slot was found for dev_addr -- the driver seam should
    // re-request the next report iff this returns true.
    bool report_received(std::uint8_t dev_addr, std::span<const std::uint8_t> report);

   private:
    std::optional<std::reference_wrapper<orb::driver::Guitar>> find_guitar(std::uint8_t dev_addr);
    std::optional<std::reference_wrapper<orb::driver::Guitar>> first_free_slot();

    std::array<orb::driver::Guitar, 2> guitars_;
};

}  // namespace orb::service
