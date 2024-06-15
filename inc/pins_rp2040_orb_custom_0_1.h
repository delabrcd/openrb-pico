#pragma once

#include "pins_common.h"

// Pin definitions taken from:
//    https://learn.adafruit.com/assets/100337

// LEDs
#define PIN_LED (23u)

// NeoPixel
#define PIN_NEOPIXEL (21u)
#define NEOPIXEL_POWER (20u)

// 'Boot0' button also on GPIO #7
#define PIN_BUTTON (7u)

// USB host connector
#define PIN_USB_HOST_DP (16u)
#define PIN_USB_HOST_DM (17u)
#define PIN_5V_EN (18u)

#define MIDI_UART uart0
#define MIDI_UART_TX (0)
#define MIDI_UART_RX (1)

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

#define SERIAL_HOWMANY (2u)
#define SPI_HOWMANY (1u)
#define WIRE_HOWMANY (1u)
