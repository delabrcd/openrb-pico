/*
 * orb::hal — GPIO interface (portable; NO SDK).
 *
 * This header defines the *contract* a platform's GPIO types must satisfy, as C++20
 * concepts. The contract is all the logic/driver layers ever see; the concrete type is
 * supplied by platform/<mcu>/ and selected in hal/platform.hpp. Binding is compile-time
 * (a concrete type, not a virtual interface) so there is ZERO runtime cost — one MCU per
 * build, a vtable indirection on a hot path would buy nothing. See docs/architecture/
 * modern-cpp.md.
 *
 * Porting: implement these concepts for the new MCU under platform/<mcu>/ and point the
 * aliases in hal/platform.hpp at it. A static_assert (below, on the selected types) turns
 * a missing/!wrong operation into a compile error at the boundary, not a runtime surprise.
 */
#pragma once

#include <concepts>

namespace orb::hal {

// A push-pull digital output pin (e.g. the status LED, the host 5V enable).
template <typename T>
concept OutputPin = requires(T pin, bool level) {
    { pin.set(level) };  // drive the pin to `level`
    { pin.high() };      // drive high
    { pin.low() };       // drive low
};

// An output that can also be released to high-impedance. Required for the CH334R hub
// RESET# line, which must be driven LOW to assert and then released to Hi-Z (never driven
// HIGH -- driving it high trips the hub's CDP charging mode; see PORTING notes). release()
// makes that invariant explicit in the type instead of a comment on a raw gpio call.
template <typename T>
concept OpenDrainPin = requires(T pin) {
    { pin.assert_low() };  // drive low (active)
    { pin.release() };     // release to Hi-Z (inactive) -- NOT driven high
};

}  // namespace orb::hal

