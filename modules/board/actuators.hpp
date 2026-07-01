#pragma once

#include <optional>

#include "hal/platform.hpp"  // orb::hal::GpioOut, orb::hal::GpioOd, orb::hal::Clock

namespace orb::board {

// Board GPIO actuation: the auth/status LED, the CH334R hub RESET# line (CUSTOM board), and
// the 5V enable (FEATHER board). Owned by orb::app::System as a plain member and injected
// where needed; replaces the file-static s_led / s_hub_rst / s_5v_en in main.cpp.
//
// The GPIO objects are std::optional and emplaced in init_led() / on first use because their
// constructors touch the chip and must not run before main() sets the system clock (the
// hardware-object lifetime rule -- same idiom the old file statics followed). The
// board-conditional method bodies (#if ORB_BOARD_ID) live in actuators.cpp, which is compiled
// per-board straight into each executable (board/CMakeLists.txt -> ORB_BOARD_BOARD_SOURCES,
// cmake/AddBoardTarget.cmake) -- this header stays board-agnostic.
class Actuators {
   public:
    // Emplace the LED (push-pull, start low == auth not yet established). Call once, post-clock.
    void init_led();

    // Auth/status LED.
    void set_auth_led(bool on);

    // Host 5V enable (FEATHER only; no-op on boards without a firmware-controllable rail).
    void set_usb_host(bool on);

    // Pulse the CH334R hub's active-low RESET# (CUSTOM board only; no-op elsewhere). Pure
    // busy-wait timing -- safe on core1 and before the scheduler. Emplaces the pin on first use.
    void reset_usb_hub();

   private:
    std::optional<orb::hal::GpioOut> led_;      // auth/status LED
    std::optional<orb::hal::GpioOd> hub_rst_;   // CH334R RESET# (CUSTOM board only)
    std::optional<orb::hal::GpioOut> v5_en_;    // 5V enable (FEATHER board only)
};

}  // namespace orb::board
