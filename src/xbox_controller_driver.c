#include <stdbool.h>
#include "common/tusb_verify.h"
#include "tusb_option.h"

#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "xbox_controller_driver.h"

// Official controllers
#define XBOX_VID1 0x045E       // Microsoft Corporation
#define XBOX_ONE_PID1 0x02D1   // Microsoft X-Box One pad
#define XBOX_ONE_PID2 0x02DD   // Microsoft X-Box One pad (Firmware 2015)
#define XBOX_ONE_PID3 0x02E3   // Microsoft X-Box One Elite pad
#define XBOX_ONE_PID4 0x02EA   // Microsoft X-Box One S pad
#define XBOX_ONE_PID13 0x0B0A  // Microsoft X-Box One Adaptive Controller
#define XBOX_ONE_PID14 0x0B12  // Microsoft X-Box Core Controller

/* Names we give to the 3 XboxONE pipes */
#define XBOX_ONE_CONTROL_PIPE 0
#define XBOX_ONE_OUTPUT_PIPE 1
#define XBOX_ONE_INPUT_PIPE 2

#define XBOX_MAX_CONTROLLERS 1
#define XBOX_ONE_MAX_ENDPOINTS 2

typedef struct {
    uint8_t daddr;

    uint8_t itf_num;

    uint8_t ep_in;
    uint8_t ep_out;

    uint16_t epin_size;
    uint16_t epout_size;

    uint16_t VID;
    uint16_t PID;

    CFG_TUH_MEM_ALIGN xbox_packet_t epin_buf;
    CFG_TUH_MEM_ALIGN xbox_packet_t epout_buf;
} xbox_interface_t;

CFG_TUH_MEM_SECTION
tu_static xbox_interface_t _xbox_itf[XBOX_MAX_CONTROLLERS];

static xbox_interface_t *find_new_itf(void) {
    for (uint8_t i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        if (_xbox_itf[i].daddr == 0)
            return &_xbox_itf[i];
    }

    return NULL;
}

TU_ATTR_ALWAYS_INLINE static inline xbox_interface_t *get_xbox_itf(uint8_t daddr, uint8_t idx) {
    TU_ASSERT(daddr > 0 && idx < XBOX_MAX_CONTROLLERS, NULL);
    xbox_interface_t *p_hid = &_xbox_itf[idx];
    return (p_hid->daddr == daddr) ? p_hid : NULL;
}

static uint8_t get_idx_by_epaddr(uint8_t daddr, uint8_t ep_addr) {
    for (uint8_t idx = 0; idx < XBOX_MAX_CONTROLLERS; idx++) {
        xbox_interface_t const *p_hid = &_xbox_itf[idx];

        if (p_hid->daddr == daddr && (p_hid->ep_in == ep_addr || p_hid->ep_out == ep_addr)) {
            return idx;
        }
    }

    return TUSB_INDEX_INVALID_8;
}

uint8_t xbox_itf_get_index(uint8_t daddr, uint8_t itf_num) {
    for (uint8_t idx = 0; idx < XBOX_MAX_CONTROLLERS; idx++) {
        xbox_interface_t const *p_hid = &_xbox_itf[idx];

        if (p_hid->daddr == daddr && p_hid->itf_num == itf_num)
            return idx;
    }

    return TUSB_INDEX_INVALID_8;
}

#if CFG_TUSB_DEBUG
static bool print_interface(const tusb_desc_interface_t *desc_itf, uint8_t daddr) {
    TU_LOG3("bLength: %d\r\n", desc_itf->bLength);
    TU_LOG3("bDescriptorType: %d\r\n", desc_itf->bDescriptorType);
    TU_LOG3("bInterfaceNumber: %d\r\n", desc_itf->bInterfaceNumber);
    TU_LOG3("bAlternateSetting: %d\r\n", desc_itf->bAlternateSetting);
    TU_LOG3("bNumEndpoints: %d\r\n", desc_itf->bNumEndpoints);
    TU_LOG3("bInterfaceSubClass: %d\r\n", desc_itf->bInterfaceSubClass);
    TU_LOG3("bInterfaceProtocol: %d\r\n", desc_itf->bInterfaceProtocol);
    TU_LOG3("iInterface: %d\r\n", desc_itf->iInterface);

    uint8_t const *p_desc               = (uint8_t const *)desc_itf;
    p_desc                              = tu_desc_next(p_desc);
    tusb_desc_endpoint_t const *desc_ep = (tusb_desc_endpoint_t const *)p_desc;

    for (int i = 0; i < desc_itf->bNumEndpoints; i++) {
        TU_ASSERT(TUSB_DESC_ENDPOINT == desc_ep->bDescriptorType);
        TU_ASSERT(tuh_edpt_open(daddr, desc_ep));
        if (tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN) {
            TU_LOG3("\tDirection: IN\r\n");
        } else {
            TU_LOG3("\tDirection: OUT\r\n");
        }
        TU_LOG3("\tbLength: %d\r\n", desc_ep->bLength);
        TU_LOG3("\tbDescriptorType: %d\r\n", desc_ep->bDescriptorType);
        TU_LOG3("\tbEndpointAddress: %02x\r\n", desc_ep->bEndpointAddress);
        TU_LOG3("\twMaxPacketSize: %d\r\n", desc_ep->wMaxPacketSize);
        TU_LOG3("\tbInterval: %d\r\n", desc_ep->bInterval);

        p_desc  = tu_desc_next(p_desc);
        desc_ep = (tusb_desc_endpoint_t const *)p_desc;
    }
    return true;
}
#endif

static bool xbox_valid_controller(uint16_t vid, uint16_t pid) {
    switch (vid) {
        case XBOX_VID1:
            switch (pid) {
                case XBOX_ONE_PID1:
                case XBOX_ONE_PID2:
                case XBOX_ONE_PID3:
                case XBOX_ONE_PID4:
                case XBOX_ONE_PID13:
                case XBOX_ONE_PID14:
                    TU_LOG3("Valid Controller: PID: %04x VID: %04x\r\n", vid, pid);
                    return true;
                default:
                    break;
            }
        default:
            break;
    }
    return false;
}

static void wait_for_tx_complete(uint8_t dev_addr, uint8_t ep_out) {
    while (usbh_edpt_busy(dev_addr, ep_out)) tuh_task();
}

bool xboxh_send_report(uint8_t daddr, uint8_t idx, const void *report, uint16_t len) {
    TU_LOG_USBH("XBOX Send Report %d\r\n", len);

    xbox_interface_t *p_hid = get_xbox_itf(daddr, idx);
    TU_VERIFY(p_hid);

    if (p_hid->ep_out == 0) {
        return false;
    } else if (len > XBOX_ONE_EP_MAXPKTSIZE) {
        // ep_out buffer is not large enough to hold contents
        return false;
    }

    // claim endpoint
    TU_VERIFY(usbh_edpt_claim(daddr, p_hid->ep_out));

    memcpy(&p_hid->epout_buf.buffer, report, len);

    TU_LOG3_MEM(p_hid->epout_buf.buffer, len, 2);

    if (!usbh_edpt_xfer(daddr, p_hid->ep_out, p_hid->epout_buf.buffer, len)) {
        usbh_edpt_release(daddr, p_hid->ep_out);
        return false;
    }

    return true;
}

bool xboxh_receive_report(uint8_t daddr, uint8_t idx) {
    xbox_interface_t *p_controller = get_xbox_itf(daddr, idx);
    TU_VERIFY(p_controller);

    // claim endpoint
    TU_VERIFY(usbh_edpt_claim(daddr, p_controller->ep_in));

    if (!usbh_edpt_xfer(daddr, p_controller->ep_in, p_controller->epin_buf.buffer,
                        p_controller->epin_size)) {
        usbh_edpt_release(daddr, p_controller->ep_in);
        return false;
    }

    return true;
}

void xboxh_init(void) {
    tu_memclr(_xbox_itf, sizeof(_xbox_itf));
}

bool xboxh_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf,
                uint16_t max_len) {
    (void)rhport;
    (void)max_len;

    TU_LOG_USBH("Trying XBOX Controller with Interface %u\r\n", desc_itf->bInterfaceNumber);

    TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == desc_itf->bInterfaceClass);
    TU_VERIFY(desc_itf->bNumEndpoints >= 2);

    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    TU_VERIFY(xbox_valid_controller(vid, pid));

#if CFG_TUSB_DEBUG
    print_interface(desc_itf, dev_addr);
#endif
    xbox_interface_t *p_controller = find_new_itf();
    TU_ASSERT(p_controller);

    uint8_t const *p_desc               = (uint8_t const *)desc_itf;
    p_desc                              = tu_desc_next(p_desc);
    tusb_desc_endpoint_t const *desc_ep = (tusb_desc_endpoint_t const *)p_desc;

    for (int i = 0; i < desc_itf->bNumEndpoints; i++) {
        TU_ASSERT(TUSB_DESC_ENDPOINT == desc_ep->bDescriptorType);
        TU_ASSERT(tuh_edpt_open(dev_addr, desc_ep));
        if (tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN) {
            uint16_t pkt_size = tu_edpt_packet_size(desc_ep);
            TU_ASSERT(pkt_size == XBOX_ONE_EP_MAXPKTSIZE);
            p_controller->ep_in     = desc_ep->bEndpointAddress;
            p_controller->epin_size = pkt_size;
        } else {
            uint16_t pkt_size = tu_edpt_packet_size(desc_ep);
            TU_ASSERT(pkt_size == XBOX_ONE_EP_MAXPKTSIZE);
            p_controller->ep_out     = desc_ep->bEndpointAddress;
            p_controller->epout_size = pkt_size;
        }

        p_desc  = tu_desc_next(p_desc);
        desc_ep = (tusb_desc_endpoint_t const *)p_desc;
    }
    p_controller->itf_num = desc_itf->bInterfaceNumber;
    p_controller->daddr   = dev_addr;
    p_controller->PID     = pid;
    p_controller->VID     = vid;
    return true;
}

static const uint8_t xboxone_s_init[] = {0x05, 0x20, 0x00, 0x0f, 0x06};

static const power_report_t power = {.data = {.frame = {.command  = CMD_POWER_MODE,
                                                        .deviceId = 0,
                                                        .type     = TYPE_REQUEST,
                                                        .sequence = 0,
                                                        .length   = 1},
                                              .data  = 0}};

bool xboxh_set_config(uint8_t daddr, uint8_t itf_num) {
    TU_LOG_USBH("XBOX Set Config addr: %02x interface: %d", daddr, itf_num);

    xbox_interface_t *p_hid = get_xbox_itf(daddr, itf_num);
    TU_VERIFY(p_hid);

    uint8_t idx = xbox_itf_get_index(daddr, itf_num);
    TU_ASSERT(xboxh_send_report(daddr, idx, power.buffer, sizeof(power)));

    wait_for_tx_complete(daddr, p_hid->ep_out);

    if ((p_hid->PID == 0x02ea || p_hid->PID == 0x0b00 || p_hid->PID == 0x0b12)) {
        TU_ASSERT(xboxh_send_report(daddr, idx, xboxone_s_init, sizeof(xboxone_s_init)));
    }

    usbh_driver_set_config_complete(daddr, itf_num);

    if (xboxh_mount_cb)
        xboxh_mount_cb(daddr, idx);
    TU_ASSERT(xboxh_receive_report(daddr, idx));
    return true;
}

bool xboxh_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)result;

    uint8_t const dir = tu_edpt_dir(ep_addr);
    uint8_t const idx = get_idx_by_epaddr(daddr, ep_addr);

    xbox_interface_t *p_controller = get_xbox_itf(daddr, idx);
    TU_VERIFY(p_controller);

    if (dir == TUSB_DIR_IN) {
        TU_LOG_USBH("  Get Report callback (%u, %u)\r\n", daddr, idx);
        p_controller->epin_buf.length = xferred_bytes;
        p_controller->epin_buf.triggered_time = 0;
        p_controller->epin_buf.handled = 0;
        if (xboxh_packet_received_cb)
            xboxh_packet_received_cb(idx, &p_controller->epin_buf, xferred_bytes);

        // xbox interface requires active polling
        TU_ASSERT(xboxh_receive_report(daddr, idx));
    } else {
        p_controller->epout_buf.length = xferred_bytes;
        if (xboxh_packet_sent_cb)
            xboxh_packet_sent_cb(idx, &p_controller->epout_buf, xferred_bytes);
    }
    return true;
}

void xboxh_close(uint8_t daddr) {
    for (uint8_t i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        xbox_interface_t *p_controller = &_xbox_itf[i];
        if (!p_controller)
            continue;
        if (p_controller->daddr == daddr) {
            p_controller->daddr = 0;
            if (xboxh_umount_cb)
                xboxh_umount_cb(daddr, i);
        }
    }
}
