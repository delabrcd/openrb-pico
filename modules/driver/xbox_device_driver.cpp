#include "adapter.h"
#include "bsp/board_api.h"
#include "hal/platform.hpp"
#include "hardware/gpio.h"
#include "orb_debug.h"
#include "tusb_option.h"
#include "xbox_one_protocol.h"

#if (CFG_TUD_ENABLED && CFG_TUD_XINPUT)

#include <array>
#include <chrono>
#include <cstdint>
#include <utility>  // std::to_underlying

#include "class/hid/hid.h"
#include "common/tusb_common.h"
#include "common/tusb_types.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "xbox_device_driver.h"

// packet_queue.h (xbox_fifo_read et al.) and xbox_one_protocol.h (included above) are C++
// headers now -- their functions are plain C++ free functions -- so include normally.
#include "packet_queue.h"

// only need a fifo for sent packets
#define XBOXD_N_BUF 15
#define XBOXD_TX_FIFO_SIZE CFG_TUD_XINPUT_TX_BUFSIZE *XBOXD_N_BUF

namespace {

struct xinputd_interface_t {
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;

    CFG_TUSB_MEM_ALIGN XboxPacket epin_buf;
    CFG_TUSB_MEM_ALIGN XboxPacket epout_buf;
};

CFG_TUSB_MEM_SECTION std::array<xinputd_interface_t, CFG_TUD_XINPUT> _xinputd_itf;

xinputd_interface_t *find_new_itf() {
    for (auto &itf : _xinputd_itf) {
        if (itf.ep_in == 0 && itf.ep_out == 0) return &itf;
    }

    return nullptr;
}

bool _xboxd_send(uint8_t itf, uint8_t *report, uint8_t len) {
    xinputd_interface_t *p_xinput = &_xinputd_itf[itf];

    len = tu_min8(len, CFG_TUD_XINPUT_TX_BUFSIZE);

    return usbd_edpt_xfer(TUD_OPT_RHPORT, p_xinput->ep_in, report, len);
}

// special! gip device request - pulled from GIMX firmewares for xbone
uint8_t request0x90_index_0x04[] = {
        0x28, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x58, 0x47, 0x49, 0x50, 0x31, 0x30, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

}  // namespace

extern "C" {

bool tud_xinput_n_ready(uint8_t itf) {
    uint8_t const ep_in = _xinputd_itf[itf].ep_in;
    return tud_ready() && (ep_in != 0) && !usbd_edpt_busy(TUD_OPT_RHPORT, ep_in);
}

bool xboxd_send(XboxPacket *packet) {
    if (!_xinputd_itf[0].ep_in) return false;

    LOG_TRC(CAT_WIRE, "sending %s size: %d", get_command_name(std::to_underlying(packet->frame().command)),
            packet->length);

    return _xboxd_send(0, packet->data(), packet->length);
}

bool xboxd_send_task() {
    XboxPacket *pkt = &_xinputd_itf[0].epin_buf;
    if (pkt->handled) {
        TU_VERIFY(xbox_fifo_read(pkt));
    }

    TU_VERIFY((orb::hal::Clock{}.now().time_since_epoch() - pkt->triggered_time) >
              orb::service::on_delay);

    TU_VERIFY(usbd_edpt_claim(0, _xinputd_itf[0].ep_in));

    if (!xboxd_send(pkt)) {
        LOG_ERR(CAT_DEV, "FAILED TO SEND %s", get_command_name(std::to_underlying(pkt->frame().command)));
        usbd_edpt_release(0, _xinputd_itf[0].ep_in);
    }
    return true;
}

//--------------------------------------------------------------------+
// USBD-CLASS API
//--------------------------------------------------------------------+
void xboxd_init(void) {
    tu_memclr(_xinputd_itf.data(), sizeof(_xinputd_itf));
    _xinputd_itf[0].epin_buf.handled = 1;
}

// CDD NOTE - with my Pi Pico santroller & my active USB extension I see a bus reset often when i
// plug it in. I'm assuming its just inrush but we gotta reset ourselves gracefully anyways to
// recover - currently this doesn't work but should once the reset_controller function is fixed
void xboxd_reset(uint8_t rhport) {
    LOG_INFO(CAT_DEV, "Resetting Device");
    (void)rhport;
    tu_memclr(_xinputd_itf.data(), sizeof(_xinputd_itf));
    _xinputd_itf[0].epin_buf.handled = 1;

    if (xboxd_on_reset_cb) xboxd_on_reset_cb();
}

uint16_t xboxd_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
    TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == itf_desc->bInterfaceClass);
    TU_VERIFY(itf_desc->bInterfaceProtocol == 0xD0);

    // HACK!!!! our first interface is subclass 0x47 but following ones are 0xFF,
    // this driver only supports the former but needs to trick TUSB into thinking
    // that we support all 3 of them so we don't stall the endpoint in
    // `process_control_requst` - I've only ever seen the xbox side driver try and
    // switch to the other interfaces if there's something misconfigured with the
    // first one so lets hope and pray we never need to use these
    if (itf_desc->bInterfaceSubClass != 0x47) {
        uint16_t drv_len = itf_desc->bLength;
        uint8_t const *p_desc = reinterpret_cast<uint8_t const *>(itf_desc);
        p_desc = tu_desc_next(p_desc);

        // support more than one interface of the same number
        tusb_desc_interface_t const *itf = reinterpret_cast<tusb_desc_interface_t const *>(p_desc);
        while (tu_desc_type(p_desc) == TUSB_DESC_INTERFACE &&
               itf->bInterfaceNumber == itf_desc->bInterfaceNumber) {
            itf = reinterpret_cast<tusb_desc_interface_t const *>(p_desc);
            drv_len += itf->bLength;
            p_desc = tu_desc_next(p_desc);
            TU_ASSERT(p_desc);
        }

        // the final interface should be where the endpoints are
        tusb_desc_endpoint_t const *desc_ep;
        for (int i = 0; i < itf->bNumEndpoints; i++) {
            desc_ep = reinterpret_cast<tusb_desc_endpoint_t const *>(p_desc);
            TU_ASSERT(TUSB_DESC_ENDPOINT == desc_ep->bDescriptorType);
            drv_len += desc_ep->bLength;
            p_desc = tu_desc_next(p_desc);
            TU_ASSERT(p_desc);
        }
        return drv_len;
    }

    // if we've made it here that means we're on the first interface - claim
    // those endpoints and store their addresses - inform TUSB about how much of
    // the config this accounts for - hardcoded

    uint16_t drv_len = sizeof(tusb_desc_interface_t) +
                       (itf_desc->bNumEndpoints * sizeof(tusb_desc_endpoint_t));

    TU_VERIFY(max_len >= drv_len, 0);

    xinputd_interface_t *p_xinput = find_new_itf();
    TU_VERIFY(p_xinput);

    p_xinput->itf_num = itf_desc->bInterfaceNumber;

    uint8_t const *p_desc = reinterpret_cast<uint8_t const *>(itf_desc);
    p_desc = tu_desc_next(p_desc);
    TU_ASSERT(tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT);
    tusb_desc_endpoint_t const *desc_ep = reinterpret_cast<tusb_desc_endpoint_t const *>(p_desc);

    for (int i = 0; i < itf_desc->bNumEndpoints; i++) {
        TU_ASSERT(TUSB_DESC_ENDPOINT == desc_ep->bDescriptorType);
        TU_ASSERT(usbd_edpt_open(rhport, desc_ep));
        if (tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN) {
            uint16_t pkt_size = tu_edpt_packet_size(desc_ep);
            TU_ASSERT(pkt_size <= CFG_TUD_XINPUT_RX_BUFSIZE);
            p_xinput->ep_in = desc_ep->bEndpointAddress;
        } else {
            uint16_t pkt_size = tu_edpt_packet_size(desc_ep);
            TU_ASSERT(pkt_size <= CFG_TUD_XINPUT_TX_BUFSIZE);
            p_xinput->ep_out = desc_ep->bEndpointAddress;
        }
        p_desc = tu_desc_next(p_desc);
        desc_ep = reinterpret_cast<tusb_desc_endpoint_t const *>(p_desc);
    }
    // Prime the OUT endpoint: receive one max packet into the 64-byte wire buffer. Length is
    // the wire capacity (== CFG_TUD_XINPUT_RX_BUFSIZE), NOT sizeof(XboxPacket) -- the latter
    // includes the host-side bookkeeping tail and, now that the wire buffer no longer sits at
    // the object's front, would let a >64-byte transfer write past epout_buf. Matches the
    // re-arm in xboxd_xfer_cb below.
    TU_ASSERT(usbd_edpt_xfer(rhport, p_xinput->ep_out, p_xinput->epout_buf.data(),
                             XboxPacket::capacity()));
    return drv_len;
}

bool xboxd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {
        if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
            if (request->bRequest == TUSB_REQ_GET_INTERFACE) {
                static uint8_t data[] = {0x00};
                (tud_control_xfer(rhport, request, data, sizeof(data)));
                return true;
            }
        } else {
            if (request->bRequest == TUSB_REQ_SET_INTERFACE) {
                (tud_control_status(rhport, request));
                return true;
            }
        }
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
        if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
            if (request->bRequest == 0x90) {
                if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE) {
                    if (request->wIndex == 0x0004) {
                        (tud_control_xfer(rhport, request, &request0x90_index_0x04,
                                          request->wLength));
                        return true;
                    }
                } else if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {
                    if (request->wIndex == 0x0005) {
                        // tud_control_status(rhport, request);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    TU_LOG_USBD("vendor request %d\r\n", stage);
    return xboxd_control_xfer_cb(rhport, stage, request);
}

bool xboxd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)result;
    (void)xferred_bytes;
    xinputd_interface_t *p_xinput = nullptr;

    for (auto &itf : _xinputd_itf) {
        if (ep_addr == itf.ep_out || ep_addr == itf.ep_in) {
            p_xinput = &itf;
            break;
        }
    }

    if (p_xinput == nullptr) return false;

    if (ep_addr == p_xinput->ep_out) {
        LOG_TRC(CAT_WIRE, "IN (%s)", get_command_name(std::to_underlying(p_xinput->epout_buf.frame().command)));
        LOG_HEXDUMP(CAT_WIRE, LOG_LEVEL_TRACE, p_xinput->epout_buf.data(), xferred_bytes);

        p_xinput->epout_buf.length = xferred_bytes;
        // TODO(rx-decouple): stop invoking the consumer synchronously here -- this pins the
        // device state machine to tud_task/core0. Instead enqueue epout_buf into a bounded RX
        // queue (non-blocking, drop-if-full, like the #47 instrument-event queue) and let a
        // separate consumer task drain it, so receivers aren't tied to this callback's
        // core/task. The handler already only writes to the tx fifo / host_tx queue + state
        // (no tud_* calls), so it is safe on any core0 task. See the "Fully decouple USB RX"
        // task; fold into P4 (DeviceSession).
        if (xboxd_packet_received_cb)
            xboxd_packet_received_cb(rhport, &p_xinput->epout_buf, xferred_bytes);
        TU_ASSERT(usbd_edpt_xfer(rhport, p_xinput->ep_out, p_xinput->epout_buf.data(),
                                 XboxPacket::capacity()));

    } else if (ep_addr == p_xinput->ep_in) {
        LOG_TRC(CAT_WIRE, "OUT (%s)", get_command_name(std::to_underlying(p_xinput->epin_buf.frame().command)));
        LOG_HEXDUMP(CAT_WIRE, LOG_LEVEL_TRACE, p_xinput->epin_buf.data(), xferred_bytes);
        p_xinput->epin_buf.handled = 1;
    }
    return true;
}

}  // extern "C"

static usbd_class_driver_t const _xboxd_driver = {
#if CFG_TUSB_DEBUG >= 2
        .name = "XBOXD",
#else
        .name = nullptr,
#endif
        .init = xboxd_init,
        .deinit = nullptr,
        .reset = xboxd_reset,
        .open = xboxd_open,
        .control_xfer_cb = xboxd_control_xfer_cb,
        .xfer_cb = xboxd_xfer_cb,
        .xfer_isr = nullptr,
        .sof = nullptr};

// Implement callback to add our custom driver
extern "C" usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &_xboxd_driver;
}

#endif
