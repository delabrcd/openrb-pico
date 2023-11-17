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

inline constexpr uint8_t D0 = (0u);
inline constexpr uint8_t D1 = (1u);
inline constexpr uint8_t D2 = (2u);
inline constexpr uint8_t D3 = (3u);
inline constexpr uint8_t D4 = (4u);
inline constexpr uint8_t D5 = (5u);
inline constexpr uint8_t D6 = (6u);
inline constexpr uint8_t D7 = (7u);
inline constexpr uint8_t D8 = (8u);
inline constexpr uint8_t D9 = (9u);
inline constexpr uint8_t D10 = (10u);
inline constexpr uint8_t D11 = (11u);
inline constexpr uint8_t D12 = (12u);
inline constexpr uint8_t D13 = (13u);
inline constexpr uint8_t D14 = (14u);
inline constexpr uint8_t D15 = (15u);
inline constexpr uint8_t D16 = (16u);
inline constexpr uint8_t D17 = (17u);
inline constexpr uint8_t D18 = (18u);
inline constexpr uint8_t D19 = (19u);
inline constexpr uint8_t D20 = (20u);
inline constexpr uint8_t D21 = (21u);
inline constexpr uint8_t D22 = (22u);
inline constexpr uint8_t D23 = (23u);
inline constexpr uint8_t D24 = (24u);
inline constexpr uint8_t D25 = (25u);
inline constexpr uint8_t D26 = (26u);
inline constexpr uint8_t D27 = (27u);
inline constexpr uint8_t D28 = (28u);
inline constexpr uint8_t D29 = (29u);

#ifdef __PIN_A0
static const uint8_t A0 = __PIN_A0;
#else
inline constexpr uint8_t A0 = (26u);
#endif

#ifdef __PIN_A1
static const uint8_t A1 = __PIN_A1;
#else
inline constexpr uint8_t A1 = (27u);
#endif

#ifdef __PIN_A2
static const uint8_t A2 = __PIN_A2;
#else
inline constexpr uint8_t A2 = (28u);
#endif

#ifdef __PIN_A3
static const uint8_t A3 = __PIN_A3;
#else
inline constexpr uint8_t A3 = (29u);
#endif

inline constexpr uint8_t SS = PIN_SPI0_SS;
inline constexpr uint8_t MOSI = PIN_SPI0_MOSI;
inline constexpr uint8_t MISO = PIN_SPI0_MISO;
inline constexpr uint8_t SCK = PIN_SPI0_SCK;

inline constexpr uint8_t SDA = PIN_WIRE0_SDA;
inline constexpr uint8_t SCL = PIN_WIRE0_SCL;
