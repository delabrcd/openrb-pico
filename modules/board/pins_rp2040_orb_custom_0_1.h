#pragma once

#include "pins_common.h"

// Pin definitions taken from:
//    https://learn.adafruit.com/assets/100337

// LEDs (PIN_LED -> orb::board::pin_led, below)

// NeoPixel
#define PIN_NEOPIXEL (21u)
#define NEOPIXEL_POWER (20u)

// 'Boot0' button also on GPIO #7
#define PIN_BUTTON (7u)

// USB host connector (PIN_USB_HOST_DP -> orb::board, below)
#define PIN_USB_HOST_DM (17u)
// GPIO18 is wired to the CH334R hub's active-low RESET# (net USB_HUB_RST) on the
// rev 0.2 board -- NOT a 5V enable (that label is vestigial from the FEATHER
// design). Pulsed low at startup to reset the hub on every boot (warm resets
// included), since the hub stays powered across an RP2040 reset.
// (-> orb::board::pin_usb_hub_rst, below.)

#define MIDI_UART uart0

#define DBG_UART_ID uart1
#define DBG_UART_TX_PIN (24)
#define DBG_UART_RX_PIN (25)

// SPI
#define PIN_SPI0_MISO (8u)
#define PIN_SPI0_MOSI (15u)
#define PIN_SPI0_SCK (14u)
#define PIN_SPI0_SS (13u)
#define __SPI0_DEVICE spi1

// Not pinned out
#define PIN_SPI1_MISO (31u)
#define PIN_SPI1_MOSI (31u)
#define PIN_SPI1_SCK (31u)
#define PIN_SPI1_SS (31u)
#define __SPI1_DEVICE spi0

// Wire
#define PIN_WIRE0_SDA (2u)
#define PIN_WIRE0_SCL (3u)
#define __WIRE0_DEVICE i2c1

#define PIN_WIRE1_SDA (31u)
#define PIN_WIRE1_SCL (31u)
#define __WIRE1_DEVICE i2c0

// SERIAL_HOWMANY / SPI_HOWMANY / WIRE_HOWMANY deleted: dead Arduino-variant macros with no
// consumer in modules/ or external/ (verified). (PINS_COUNT / NUM_* / ADC_RESOLUTION were
// removed from pins_common.h for the same reason.)

#ifdef __cplusplus
// Pin numbers our code actually drives, as typed constexpr in the per-board namespace
// (BUCKET A). `unsigned` reproduces the old `(Nu)` macro literal type exactly so codegen is
// byte-identical. Hardware-instance selectors (MIDI_UART, DBG_UART_ID, __*_DEVICE) stay
// macros -- they name pico-sdk objects, not pin numbers, and must expand lazily at use.
namespace orb::board {
inline constexpr unsigned pin_led = 23u;
inline constexpr unsigned pin_usb_host_dp = 16u;
inline constexpr unsigned pin_usb_hub_rst = 18u;
inline constexpr unsigned midi_uart_tx = 0u;
inline constexpr unsigned midi_uart_rx = 1u;
}  // namespace orb::board
#endif  // __cplusplus
