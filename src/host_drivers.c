#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "tusb_option.h"
#include "usb_midi_host.h"
#include "util.h"
#include "xbox_controller_driver.h"
// #include "xinput_host.h"

const usbh_class_driver_t _drivers[] = {{
#if CFG_TUSB_DEBUG >= 2
                                                .name = "MIDIH",
#endif
                                                .init = midih_init,
                                                .open = midih_open,
                                                .set_config = midih_set_config,
                                                .xfer_cb = midih_xfer_cb,
                                                .close = midih_close},
                                        {
#if CFG_TUSB_DEBUG >= 2
                                                .name = "XBOXH",
#endif
                                                .init = xboxh_init,
                                                .open = xboxh_open,
                                                .set_config = xboxh_set_config,
                                                .xfer_cb = xboxh_xfer_cb,
                                                .close = xboxh_close}};

// ,
//                                         {

// #if CFG_TUSB_DEBUG >= 2
//                                                 .name = "XINPUTH",
// #endif
//                                                 .init = xinputh_init,
//                                                 .open = xinputh_open,
//                                                 .set_config = xinputh_set_config,
//                                                 .xfer_cb = xinputh_xfer_cb,
//                                                 .close = xinputh_close}

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = UTIL_NUM(_drivers);
    return _drivers;
}