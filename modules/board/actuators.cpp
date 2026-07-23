/*
 * Board GPIO actuation (orb::board::Actuators). BOARD-DEPENDENT: the set_usb_host /
 * reset_usb_hub bodies are gated on ORB_BOARD_ID and pull the per-board pin map through
 * orb_bsp.h, so this TU is compiled per-board straight into each executable (like
 * app/main.cpp + service/midi.cpp) -- see board/CMakeLists.txt + cmake/AddBoardTarget.cmake.
 */
#include "actuators.hpp"

#include <chrono>

#include "orb_bsp.h"  // ORB_BOARD_ID + board pin map (pin_led, pin_usb_hub_rst, pin_5v_en)

namespace orb::board {

void Actuators::init_led() {
    // LED: push-pull output, start low (auth not yet established).
    led_.emplace(orb::board::pin_led, /*initial=*/false);
}

void Actuators::set_auth_led(bool on) {
    if (led_) led_->set(on);
}

void Actuators::set_usb_host(bool on) {
#if ORB_BOARD_ID == ORB_BOARD_ID_FEATHER
    if (!v5_en_) v5_en_.emplace(orb::board::pin_5v_en);
    v5_en_->set(on);
#else
    (void)on;
#endif
}

// The CH334R hub stays powered across an RP2040 warm/watchdog reset, so it keeps stale state
// and the downstream controller fails to re-enumerate -- only a hub RESET# pulse recovers it.
// CH334/335 datasheet (V2.5, sec 3.2 / Table 3-2): RESET#/CDP is active-low with a built-in
// ~25k pull-up; a low pulse >4us resets the chip; POR after release is ~5-14ms. CRITICAL: do
// NOT actively drive the pin HIGH -- a driven-high level as the hub exits reset enables CDP
// charging-port mode and disturbs the downstream port. Release to Hi-Z and let the internal
// pull-up bring it high (GpioOd::release()). Busy-wait delay -- pure timer wait, safe
// pre-scheduler and on core1; never sleep_ms (blocks via FreeRTOS before the scheduler -> hang).
void Actuators::reset_usb_hub() {
#if ORB_BOARD_ID == ORB_BOARD_ID_CUSTOM_REV_0_1
    // Emplace once; subsequent calls (runtime recovery) reuse the already-configured pin.
    if (!hub_rst_) hub_rst_.emplace(orb::board::pin_usb_hub_rst);
    orb::hal::Clock clk;
    hub_rst_->assert_low();                     // drive RESET# low (>4us; we hold 10ms)
    clk.delay(std::chrono::milliseconds(10));
    hub_rst_->release();                        // release to Hi-Z; external pull-up -> high
    clk.delay(std::chrono::milliseconds(50));   // wait out the hub POR (~5-14ms) before host init
#endif
}

}  // namespace orb::board
