/*
 * platform::pico — GPIO implementation. THIS is the boundary: it is allowed to include
 * the Pico SDK; nothing above the HAL is. The types here satisfy the orb::hal GPIO
 * concepts (checked by static_assert in hal/platform.hpp).
 *
 * Pins are passed at construction (runtime uint) rather than as template parameters: the
 * cost is one stored byte and the SDK gpio_* calls already take a runtime pin, so there
 * is no codegen benefit to compile-time pins, and it keeps the board pin tables simple.
 */
#pragma once

#include "hardware/gpio.h"

namespace orb::platform::pico {

// Push-pull digital output. Sets the output latch to `initial` BEFORE enabling the driver
// so there is no power-on glitch on the line.
class GpioOut {
   public:
    explicit GpioOut(unsigned pin, bool initial = false) : pin_(pin) {
        gpio_init(pin_);
        gpio_put(pin_, initial);
        gpio_set_dir(pin_, GPIO_OUT);
    }

    void set(bool level) const { gpio_put(pin_, level); }
    void high() const { gpio_put(pin_, true); }
    void low() const { gpio_put(pin_, false); }

   private:
    unsigned pin_;
};

// Open-drain-style control line (used for the CH334R hub RESET#). assert_low() drives the
// pin low; release() returns it to a high-impedance input so an external pull-up (never
// the MCU) sets the idle level -- this is the invariant the CH334R needs (driving it high
// trips CDP charging mode). reset_usb_hub()'s busy_wait timing stays in the caller.
class GpioOpenDrain {
   public:
    // Start released (Hi-Z input). The line idles via its external pull-up.
    explicit GpioOpenDrain(unsigned pin) : pin_(pin) {
        gpio_init(pin_);
        gpio_put(pin_, false);  // pre-load a low into the latch for assert_low()
        release();
    }

    void assert_low() const {
        gpio_put(pin_, false);
        gpio_set_dir(pin_, GPIO_OUT);  // now actively drives low
    }
    void release() const {
        gpio_set_dir(pin_, GPIO_IN);  // Hi-Z; external pull-up takes the line high
    }

   private:
    unsigned pin_;
};

}  // namespace orb::platform::pico

