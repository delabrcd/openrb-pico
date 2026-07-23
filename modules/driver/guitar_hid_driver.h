#pragma once

// TinyUSB HID host seam for the guitar service (orb::service::GuitarHost). Binds a single
// orb::core::SeamAnchor<GuitarHost> at init -- before tuh_init() runs on core1 -- and the
// extern "C" tuh_hid_*_cb callbacks (defined in guitar_hid_driver.cpp) forward straight into
// the service with a direct concrete call. This is the tusb-owning half of the guitar HID
// seam that used to live in modules/service/guitar.cpp; GuitarHost itself (guitar.h/.cpp)
// stays tusb-free -- see modules/core/seam_anchor.hpp for the pattern.

#include "guitar.h"  // orb::service::GuitarHost

namespace orb::driver {

void bind_guitar_hid(orb::service::GuitarHost& host);

}  // namespace orb::driver
