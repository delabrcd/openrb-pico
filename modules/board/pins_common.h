#pragma once

#include <stdint.h>

#define PINS_COUNT (30u)
#define NUM_DIGITAL_PINS (30u)
#define NUM_ANALOG_INPUTS (4u)
#define NUM_ANALOG_OUTPUTS (0u)
#define ADC_RESOLUTION (12u)

#ifdef PIN_LED
#define LED_BUILTIN PIN_LED
#endif

#define D0 (0u)
#define D1 (1u)
#define D2 (2u)
#define D3 (3u)
#define D4 (4u)
#define D5 (5u)
#define D6 (6u)
#define D7 (7u)
#define D8 (8u)
#define D9 (9u)
#define D10 (10u)
#define D11 (11u)
#define D12 (12u)
#define D13 (13u)
#define D14 (14u)
#define D15 (15u)
#define D16 (16u)
#define D17 (17u)
#define D18 (18u)
#define D19 (19u)
#define D20 (20u)
#define D21 (21u)
#define D22 (22u)
#define D23 (23u)
#define D24 (24u)
#define D25 (25u)
#define D26 (26u)
#define D27 (27u)
#define D28 (28u)
#define D29 (29u)

#ifdef __PIN_A0
static const uint8_t A0 = __PIN_A0;
#else
#define A0 (26u)
#endif

#ifdef __PIN_A1
static const uint8_t A1 = __PIN_A1;
#else
#define A1 (27u)
#endif

#ifdef __PIN_A2
static const uint8_t A2 = __PIN_A2;
#else
#define A2 (28u)
#endif

#ifdef __PIN_A3
static const uint8_t A3 = __PIN_A3;
#else
#define A3 (29u)
#endif

#define SS PIN_SPI0_SS;
#define MOSI PIN_SPI0_MOSI;
#define MISO PIN_SPI0_MISO;
#define SCK PIN_SPI0_SCK;

#define SDA PIN_WIRE0_SDA;
#define SCL PIN_WIRE0_SCL;
