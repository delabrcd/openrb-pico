/*
 * Test-side controls for the host stubs (host build only). Lets a test pin the fake
 * millisecond clock that the board_millis() stub returns, so the triggered_time stamp
 * written by fill_guitar_input_from_hid_report becomes deterministic and assertable.
 */
#ifndef OPENRB_HOST_TEST_SUPPORT_H
#define OPENRB_HOST_TEST_SUPPORT_H

#include <cstdint>

// Value returned by the host board_millis() stub. Defaults to 0; a test sets it before
// calling code that timestamps with board_millis().
extern std::uint32_t g_host_fake_millis;

// Value returned by the host hal::Clock::now_us() stub (microseconds). Defaults to 0.
// Set this before calling code that timestamps via orb::hal::Clock::now_us(). The
// triggered_time field in an xbox_packet_t is stamped as now_us() / 1000, so set this to
// the desired_ms * 1000u (must fit in uint32_t, i.e. desired_ms <= ~4,294,967 ms).
extern std::uint32_t g_host_fake_us;

#endif  // OPENRB_HOST_TEST_SUPPORT_H
