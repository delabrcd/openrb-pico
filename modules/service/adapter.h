#pragma once

#include <cstdint>

typedef enum adapter_state_e {
    STATE_NONE,
    STATE_INIT,
    STATE_IDENTIFYING,
    STATE_AUTHENTICATING,
    STATE_RUNNING,
    STATE_POWER_OFF,
} adapter_state_t;

// Runtime tunables (BUCKET A): plain object-like values, never used in `#if`, so they
// become typed constexpr -- no preprocessor needed. All consumers are C++.
namespace orb::service {

// Re-announce cadence and the input/timing thresholds, in milliseconds.
inline constexpr std::uint32_t announce_interval_ms = 2000;
inline constexpr std::uint8_t velocity_thresh = 10;
inline constexpr std::uint32_t trigger_hold_ms = 40;
inline constexpr std::uint32_t on_delay_ms = 20;

// USB endpoint-direction bits (bit 7 of bEndpointAddress).
inline constexpr std::uint8_t endpoint_dir_out = 0x00;
inline constexpr std::uint8_t endpoint_dir_in = 0x80;

// Adapter interrupt-endpoint polling interval (ms); also the drum-emit throttle.
inline constexpr std::uint8_t adapter_out_interval = 4;
inline constexpr std::uint8_t adapter_in_interval = 4;

}  // namespace orb::service
