/*
 * orb::hal — platform selection. This is the ONE place the HAL is bound to a concrete
 * platform implementation. Logic/driver code uses the `orb::hal::` aliases below and never
 * names a platform type directly, so porting to another MCU = add platform/<mcu>/ and
 * switch the includes + aliases here (e.g. behind a board/platform macro). The
 * static_asserts make a platform type that doesn't satisfy its HAL concept a COMPILE error
 * at this boundary rather than a runtime surprise.
 *
 * As HAL subsystems land (clock, uart, dma, watchdog, irq, usb-phy) they add their concept
 * + platform impl + alias + assert here, following the GPIO slice.
 */
#pragma once

#include "hal/clock.hpp"
#include "hal/gpio.hpp"
#include "hal/interrupt.hpp"
#include "hal/uart.hpp"
#include "platform/pico/clock.hpp"
#include "platform/pico/gpio.hpp"
#include "platform/pico/interrupt.hpp"
#include "platform/pico/uart.hpp"

namespace orb::hal {

// --- selected platform: RP2040 / Pico SDK ------------------------------------
using GpioOut = platform::pico::GpioOut;        // push-pull output (LED, 5V enable)
using GpioOd = platform::pico::GpioOpenDrain;   // open-drain control (CH334R hub RESET#)
using Clock = platform::pico::Clock;            // lock-free us clock + busy-delay
using Uart = platform::pico::Uart;              // raw byte UART (debug log, serial MIDI)
using IrqGuard = platform::pico::IrqGuard;      // scoped per-core interrupt mask

// --- contract enforcement (compile-time) -------------------------------------
static_assert(OutputPin<GpioOut>, "platform GpioOut must satisfy hal::OutputPin");
static_assert(OpenDrainPin<GpioOd>, "platform GpioOd must satisfy hal::OpenDrainPin");
static_assert(MonotonicClock<Clock>, "platform Clock must satisfy hal::MonotonicClock");
static_assert(BusyDelay<Clock>, "platform Clock must satisfy hal::BusyDelay");
static_assert(ByteUart<Uart>, "platform Uart must satisfy hal::ByteUart");
static_assert(ScopedIrqMask<IrqGuard>, "platform IrqGuard must satisfy hal::ScopedIrqMask");

}  // namespace orb::hal

