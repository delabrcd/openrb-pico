/*
 * TinyUSB host custom-class-driver registration table, as modern C++. TinyUSB discovers
 * application-provided host class drivers by calling usbh_app_driver_get_cb() through its C
 * symbol, so that entry point stays an extern "C" free function returning the same
 * usbh_class_driver_t const* + count as the C original. The table itself becomes a
 * constexpr std::array, but every entry is an aggregate of C function pointers (midih_* from
 * usb_midi_host.c, xboxh_* from xbox_controller_driver.c) -- the stored bytes are unchanged.
 *
 * C seam: usb_midi_host.h carries its own __cplusplus extern "C" guard, and
 * xbox_controller_driver.h self-guards its xboxh_* declarations with its own extern "C"
 * block (the genuine TinyUSB driver-class callback seam), so both are included normally;
 * the xboxh_* function pointers still resolve to their C-linkage definitions.
 */
#include <array>
#include <cstdint>

#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "tusb_option.h"
#include "usb_midi_host.h"
// #include "xinput_host.h"

#include "xbox_controller_driver.h"

namespace orb::driver {
namespace {

// One usbh_class_driver_t per application host class driver. C++20 designated initialisers
// must appear in declaration order (name, init, deinit, open, set_config, xfer_cb, close).
// Every field is named explicitly -- name/deinit are nullptr (the C99 original left them
// unset, which value-initialises identically; spelling them out avoids a C++-only
// -Wmissing-field-initializers under -Wextra). .name carries a label only on a TinyUSB
// debug build (CFG_TUSB_DEBUG >= 2), matching the original guard, so the bytes are unchanged.
constexpr std::array<usbh_class_driver_t, 2> kDrivers{{
    {
#if CFG_TUSB_DEBUG >= 2
        .name = "MIDIH",
#else
        .name = nullptr,
#endif
        .init = midih_init,
        .deinit = nullptr,
        .open = midih_open,
        .set_config = midih_set_config,
        .xfer_cb = midih_xfer_cb,
        .close = midih_close,
    },
    {
#if CFG_TUSB_DEBUG >= 2
        .name = "XBOXH",
#else
        .name = nullptr,
#endif
        .init = xboxh_init,
        .deinit = nullptr,
        .open = xboxh_open,
        .set_config = xboxh_set_config,
        .xfer_cb = xboxh_xfer_cb,
        .close = xboxh_close,
    },
    // {
    // #if CFG_TUSB_DEBUG >= 2
    //         .name = "XINPUTH",
    // #endif
    //         .init = xinputh_init,
    //         .open = xinputh_open,
    //         .set_config = xinputh_set_config,
    //         .xfer_cb = xinputh_xfer_cb,
    //         .close = xinputh_close},
}};

}  // namespace
}  // namespace orb::driver

// --- extern "C" seam: TinyUSB calls this by its C symbol to enumerate host class drivers ---
extern "C" usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = static_cast<std::uint8_t>(orb::driver::kDrivers.size());
    return orb::driver::kDrivers.data();
}
