#pragma once

#include <chrono>
#include <cstdint>

enum class adapter_state_t : std::uint8_t {
    STATE_NONE,
    STATE_INIT,
    STATE_IDENTIFYING,
    STATE_AUTHENTICATING,
    STATE_RUNNING,
    STATE_POWER_OFF,
};

// Runtime tunables (BUCKET A): plain object-like values, never used in `#if`, so they
// become typed constexpr -- no preprocessor needed. All consumers are C++.
namespace orb::service {

// Re-announce cadence and the input/timing thresholds.
inline constexpr std::chrono::milliseconds announce_interval{2000};
inline constexpr std::uint8_t velocity_thresh = 10;
inline constexpr std::chrono::milliseconds trigger_hold{40};
inline constexpr std::chrono::milliseconds on_delay{20};

// USB endpoint-direction bits (bit 7 of bEndpointAddress).
inline constexpr std::uint8_t endpoint_dir_out = 0x00;
inline constexpr std::uint8_t endpoint_dir_in = 0x80;

// Adapter interrupt-endpoint polling interval; also the drum-emit throttle. Encoded to a
// uint8 wire byte (PollingIntervalMS) at the descriptor site (usb_descriptors.cpp).
inline constexpr std::chrono::milliseconds adapter_out_interval{4};
inline constexpr std::chrono::milliseconds adapter_in_interval{4};

}  // namespace orb::service
