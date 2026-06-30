/*
 * HOST-TEST stub for hal/platform.hpp.
 *
 * The real hal/platform.hpp (modules/hal/platform.hpp) binds to pico-sdk platform types
 * (platform/pico/ -> hardware/) which cannot compile on the host. This stub is
 * placed FIRST on the host-test include path (test/host_stubs/ precedes modules/), so it
 * shadows the real header for the host build.
 *
 * Provides only orb::hal::Clock, the one type that xbox_one_protocol.cpp uses. Clock::now_us()
 * returns g_host_fake_us (a controllable test counter), so tests can pin the timestamp
 * written into xbox_packet_t::triggered_time and assert its value deterministically.
 *
 * All other hal:: types (GpioOut, GpioOd, Uart, IrqGuard) are defined as stubs here to
 * satisfy the compiler if they appear in transitive includes; the protocol TU never
 * instantiates them, so they are never called.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <span>

#include "host_test_support.h"  // g_host_fake_us

namespace orb::hal {

// Stub clock: now_us() returns the controllable fake counter set by the test.
// Satisfies hal::MonotonicClock + hal::BusyDelay (delay ops are no-ops on host).
struct Clock {
    uint32_t now_us() const { return g_host_fake_us; }
    void delay_us(uint32_t) const {}
    void delay_ms(uint32_t) const {}
};

// Minimal stubs for the remaining hal types — not used by the protocol TU but present
// so any transitive include that names them compiles cleanly on the host.
struct GpioOut {
    explicit GpioOut(unsigned, bool = false) {}
    void set(bool) const {}
    void high() const {}
    void low() const {}
};

struct GpioOd {
    explicit GpioOd(unsigned) {}
    void assert_low() const {}
    void release() const {}
};

struct Uart {
    Uart(void*, unsigned, unsigned, unsigned) {}
    void write_byte(std::byte) const {}
    void write(std::span<const std::byte>) const {}
    bool readable() const { return false; }
    std::byte read_byte() const { return std::byte{0}; }
};

struct IrqGuard {
    IrqGuard() noexcept = default;
    ~IrqGuard() noexcept = default;
    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;
};

}  // namespace orb::hal
