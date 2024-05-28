#include "adapter.h"
#include "bsp/board_api.h"
#include "hardware/gpio.h"
#include "orb_debug.h"
#include "pins_rp2040_usbh.h"
#include "tusb_option.h"
#include "xbox_one_protocol.h"

#if (CFG_TUD_ENABLED && CFG_TUD_XINPUT)

#include "class/hid/hid.h"
#include "common/tusb_common.h"
#include "common/tusb_types.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "packet_queue.h"
#include "xbox_device_driver.h"

// only need a fifo for sent packets
#define XBOXD_N_BUF 15
#define XBOXD_TX_FIFO_SIZE CFG_TUD_XINPUT_TX_BUFSIZE *XBOXD_N_BUF

typedef struct {
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;

    CFG_TUSB_MEM_ALIGN xbox_packet_t epin_buf;
    CFG_TUSB_MEM_ALIGN xbox_packet_t epout_buf;
} xinputd_interface_t;

CFG_TUSB_MEM_SECTION static xinputd_interface_t _xinputd_itf[CFG_TUD_XINPUT];

static xinputd_interface_t *find_new_itf(void) {
    for (uint8_t i = 0; i < CFG_TUD_XINPUT; i++) {
        if (_xinputd_itf[i].ep_in == 0 && _xinputd_itf[i].ep_out == 0) return &_xinputd_itf[i];
    }

    return NULL;
}

bool tud_xinput_n_ready(uint8_t itf) {
    uint8_t const ep_in = _xinputd_itf[itf].ep_in;
    return tud_ready() && (ep_in != 0) && !usbd_edpt_busy(TUD_OPT_RHPORT, ep_in);
}

static bool _xboxd_send(uint8_t itf, uint8_t *report, uint8_t len) {
    xinputd_interface_t *p_xinput = &_xinputd_itf[itf];

    len = tu_min8(len, CFG_TUD_XINPUT_TX_BUFSIZE);

    return usbd_edpt_xfer(TUD_OPT_RHPORT, p_xinput->ep_in, report, len);
}

bool xboxd_send(xbox_packet_t *packet) {
    if (!_xinputd_itf[0].ep_in) return false;

    OPENRB_DEBUG("sending %s size: %d\r\n", get_command_name(packet->frame.command),
                 packet->length);

    return _xboxd_send(0, packet->buffer, packet->length);
}

bool xboxd_send_task() {
    xbox_packet_t *pkt = &_xinputd_itf[0].epin_buf;
    if (pkt->handled) {
        TU_VERIFY(xbox_fifo_read(pkt));
    }

    TU_VERIFY((board_millis() - pkt->triggered_time) > ON_DELAY_MS);

    TU_VERIFY(usbd_edpt_claim(0, _xinputd_itf[0].ep_in));

    if (!xboxd_send(pkt)) {
        OPENRB_DEBUG("FAILED TO SEND %s\r\n", get_command_name(pkt->frame.command));
        usbd_edpt_release(0, _xinputd_itf[0].ep_in);
    }
    return true;
}

//--------------------------------------------------------------------+
// USBD-CLASS API
//--------------------------------------------------------------------+
void xboxd_init(void) {
    tu_memclr(_xinputd_itf, sizeof(_xinputd_itf));
    _xinputd_itf[0].epin_buf.handled = 1;
}

extern volatile adapter_state_t adapter_state;
extern void reset_controllers();

// CDD NOTE - with my Pi Pico santroller & my active USB extension I see a bus reset often when i
// plug it in. I'm assuming its just inrush but we gotta reset ourselves gracefully anyways to
// recover - currently this doesn't work but should once the reset_controller function is fixed
void xboxd_reset(uint8_t rhport) {
    OPENRB_DEBUG("Resetting Device\r\n");
    (void)rhport;
    tu_memclr(_xinputd_itf, sizeof(_xinputd_itf));
    _xinputd_itf[0].epin_buf.handled = 1;

    gpio_put(PIN_LED, false);
    adapter_state = STATE_NONE;
    xbox_fifo_clear();
    reset_controllers();
    adapter_state = STATE_INIT;
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
        uint8_t const *p_desc = (uint8_t const *)itf_desc;
        p_desc = tu_desc_next(p_desc);

        // support more than one interface of the same number
        tusb_desc_interface_t const *itf = (tusb_desc_interface_t *)p_desc;
        while (tu_desc_type(p_desc) == TUSB_DESC_INTERFACE &&
               itf->bInterfaceNumber == itf_desc->bInterfaceNumber) {
            itf = (tusb_desc_interface_t const *)p_desc;
            drv_len += itf->bLength;
            p_desc = tu_desc_next(p_desc);
            TU_ASSERT(p_desc);
        }

        // the final interface should be where the endpoints are
        tusb_desc_endpoint_t const *desc_ep;
        for (int i = 0; i < itf->bNumEndpoints; i++) {
            desc_ep = (tusb_desc_endpoint_t const *)p_desc;
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

    uint8_t const *p_desc = (uint8_t const *)itf_desc;
    p_desc = tu_desc_next(p_desc);
    TU_ASSERT(tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT);
    tusb_desc_endpoint_t const *desc_ep = (tusb_desc_endpoint_t const *)p_desc;

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
        desc_ep = (tusb_desc_endpoint_t const *)p_desc;
    }
    TU_ASSERT(usbd_edpt_xfer(rhport, p_xinput->ep_out, p_xinput->epout_buf.buffer,
                             sizeof(p_xinput->epout_buf)));
    return drv_len;
}

// special! gip device request - pulled from GIMX firmewares for xbone
static uint8_t request0x90_index_0x04[] = {
        0x28, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x58, 0x47, 0x49, 0x50, 0x31, 0x30, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

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
    uint8_t itf = 0;
    xinputd_interface_t *p_xinput = _xinputd_itf;

    for (;; itf++, p_xinput++) {
        if (itf >= TU_ARRAY_SIZE(_xinputd_itf)) return false;

        if (ep_addr == p_xinput->ep_out || ep_addr == p_xinput->ep_in) break;
    }

    if (ep_addr == p_xinput->ep_out) {
        OPENRB_DEBUG("IN (%s): ", get_command_name(p_xinput->epout_buf.frame.command));
        OPENRB_DEBUG_BUF(p_xinput->epout_buf.buffer, xferred_bytes);
        OPENRB_DEBUG("\n");

        p_xinput->epout_buf.length = xferred_bytes;
        if (xboxd_packet_received_cb)
            xboxd_packet_received_cb(rhport, &p_xinput->epout_buf, xferred_bytes);
        TU_ASSERT(usbd_edpt_xfer(rhport, p_xinput->ep_out, p_xinput->epout_buf.buffer,
                                 sizeof(p_xinput->epout_buf.buffer)));

    } else if (ep_addr == p_xinput->ep_in) {
        OPENRB_DEBUG("OUT (%s): ", get_command_name(p_xinput->epin_buf.frame.command));
        OPENRB_DEBUG_BUF(p_xinput->epin_buf.buffer, xferred_bytes);
        OPENRB_DEBUG("\n");
        p_xinput->epin_buf.handled = 1;
    }
    return true;
}

static usbd_class_driver_t const _xboxd_driver = {
#if CFG_TUSB_DEBUG >= 2
        .name = "XBOXD",
#endif
        .init = xboxd_init,
        .reset = xboxd_reset,
        .open = xboxd_open,
        .control_xfer_cb = xboxd_control_xfer_cb,
        .xfer_cb = xboxd_xfer_cb,
        .sof = NULL};

// Implement callback to add our custom driver
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &_xboxd_driver;
}

#endif