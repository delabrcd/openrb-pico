/*
 * TinyUSB host class driver for the Xbox One controller, enumerated over the CH334R hub on
 * the bit-banged Pico-PIO-USB core (core1). Modern-C++ internals; the wire protocol, the
 * enumeration sequence, the claim->usbh_edpt_xfer->release dance (with release on every
 * error path), the IN-error-streak recovery signal and the non-blocking tuh_task_ext(0,
 * false) TX pumps are preserved byte-for-byte and timing-for-timing -- a change here breaks
 * enumeration on the PIO-USB core (see docs/FREERTOS-PORT.md core1 rules).
 *
 * C seam: the TinyUSB host callbacks (xboxh_init/open/set_config/xfer_cb/close) are the
 * vendor seam, registered as C function pointers in host_drivers.cpp; the weak
 * xboxh_*_cb hooks and the xboxh_send_report / xboxh_reinit_controller /
 * xboxh_in_error_streak / xboxh_clear_error_streak entry points are called from main.cpp.
 * All of those keep C linkage via the ORB_C_BEGIN seam in the header. Only the file-local
 * helpers and state move into an anonymous namespace.
 */
#include "xbox_controller_driver.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <utility>  // std::to_underlying

#include "common/tusb_verify.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "orb_log.h"
#include "tusb_option.h"

namespace {

// Official controller VIDs/PIDs.
inline constexpr uint16_t kXboxVid1 = 0x045E;       // Microsoft Corporation
inline constexpr uint16_t kXboxOnePid1 = 0x02D1;    // Microsoft X-Box One pad
inline constexpr uint16_t kXboxOnePid2 = 0x02DD;    // Microsoft X-Box One pad (Firmware 2015)
inline constexpr uint16_t kXboxOnePid3 = 0x02E3;    // Microsoft X-Box One Elite pad
inline constexpr uint16_t kXboxOnePid4 = 0x02EA;    // Microsoft X-Box One S pad
inline constexpr uint16_t kXboxOnePid13 = 0x0B0A;   // Microsoft X-Box One Adaptive Controller
inline constexpr uint16_t kXboxOnePid14 = 0x0B12;   // Microsoft X-Box Core Controller

// Logical pipe indices (for reference; not used as array indices in current code).
inline constexpr uint8_t kXboxOneControlPipe = 0;
inline constexpr uint8_t kXboxOneOutputPipe = 1;
inline constexpr uint8_t kXboxOneInputPipe = 2;

inline constexpr uint8_t kXboxMaxControllers = 1;
inline constexpr uint8_t kXboxOneMaxEndpoints = 2;

struct xbox_interface_t {
    uint8_t daddr;

    uint8_t itf_num;

    uint8_t ep_in;
    uint8_t ep_out;

    uint16_t epin_size;
    uint16_t epout_size;

    uint16_t VID;
    uint16_t PID;

    bool is_powered;

    CFG_TUH_MEM_ALIGN XboxPacket epin_buf;
    CFG_TUH_MEM_ALIGN XboxPacket epout_buf;
};

CFG_TUH_MEM_SECTION
tu_static std::array<xbox_interface_t, kXboxMaxControllers> _xbox_itf;

xbox_interface_t *find_new_itf(void) {
    for (auto &itf : _xbox_itf) {
        if (itf.daddr == 0) return &itf;
    }

    return nullptr;
}

TU_ATTR_ALWAYS_INLINE static inline xbox_interface_t *get_xbox_itf(uint8_t daddr, uint8_t idx) {
    TU_ASSERT(daddr > 0 && idx < kXboxMaxControllers, nullptr);
    xbox_interface_t *p_hid = &_xbox_itf[idx];
    return (p_hid->daddr == daddr) ? p_hid : nullptr;
}

uint8_t get_idx_by_epaddr(uint8_t daddr, uint8_t ep_addr) {
    auto const it = std::ranges::find_if(_xbox_itf, [&](xbox_interface_t const &p_hid) {
        return p_hid.daddr == daddr && (p_hid.ep_in == ep_addr || p_hid.ep_out == ep_addr);
    });

    if (it == _xbox_itf.end()) return TUSB_INDEX_INVALID_8;

    return static_cast<uint8_t>(std::ranges::distance(_xbox_itf.begin(), it));
}

uint8_t xbox_itf_get_index(uint8_t daddr, uint8_t itf_num) {
    auto const it = std::ranges::find_if(_xbox_itf, [&](xbox_interface_t const &p_hid) {
        return p_hid.daddr == daddr && p_hid.itf_num == itf_num;
    });

    if (it == _xbox_itf.end()) return TUSB_INDEX_INVALID_8;

    return static_cast<uint8_t>(std::ranges::distance(_xbox_itf.begin(), it));
}

#if CFG_TUSB_DEBUG
bool print_interface(const tusb_desc_interface_t *desc_itf, uint8_t daddr) {
    TU_LOG3("bLength: %d\r\n", desc_itf->bLength);
    TU_LOG3("bDescriptorType: %d\r\n", desc_itf->bDescriptorType);
    TU_LOG3("bInterfaceNumber: %d\r\n", desc_itf->bInterfaceNumber);
    TU_LOG3("bAlternateSetting: %d\r\n", desc_itf->bAlternateSetting);
    TU_LOG3("bNumEndpoints: %d\r\n", desc_itf->bNumEndpoints);
    TU_LOG3("bInterfaceSubClass: %d\r\n", desc_itf->bInterfaceSubClass);
    TU_LOG3("bInterfaceProtocol: %d\r\n", desc_itf->bInterfaceProtocol);
    TU_LOG3("iInterface: %d\r\n", desc_itf->iInterface);

    uint8_t const *p_desc = reinterpret_cast<uint8_t const *>(desc_itf);
    p_desc = tu_desc_next(p_desc);
    tusb_desc_endpoint_t const *desc_ep = reinterpret_cast<tusb_desc_endpoint_t const *>(p_desc);

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

        p_desc = tu_desc_next(p_desc);
        desc_ep = reinterpret_cast<tusb_desc_endpoint_t const *>(p_desc);
    }
    return true;
}
#endif

bool xbox_valid_controller(uint16_t vid, uint16_t pid) {
    switch (vid) {
        case kXboxVid1:
            switch (pid) {
                case kXboxOnePid1:
                case kXboxOnePid2:
                case kXboxOnePid3:
                case kXboxOnePid4:
                case kXboxOnePid13:
                case kXboxOnePid14:
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

void wait_for_tx_complete(uint8_t dev_addr, uint8_t ep_out) {
    // Non-blocking pump (see usb_log.cpp wait_for_disk_io): under OPT_OS_FREERTOS a plain
    // tuh_task() would block on the host event queue instead of spinning the TX out.
    while (usbh_edpt_busy(dev_addr, ep_out)) tuh_task_ext(0, false);
}

bool xboxh_power_off_controller(xbox_interface_t *p_itf) {
    if (p_itf->daddr == 0) return false;
    if (!p_itf->is_powered) return false;

    power_report_t out = make_power_report(get_sequence(), 0x05);

    TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, &out, sizeof(out)));
    wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);

    out.data = std::to_underlying(power_mode_e::POWER_OFF);
    out.frame.sequence = get_sequence();

    TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, &out, sizeof(out)));
    wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);

    out.frame.sequence = get_sequence();

    TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, &out, sizeof(out)));
    wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);

    out.frame.sequence = get_sequence();

    TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, &out, sizeof(out)));
    wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);

    out.data = std::to_underlying(power_mode_e::POWER_SLEEP);
    out.frame.sequence = get_sequence();

    TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, &out, sizeof(out)));
    wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);
    p_itf->is_powered = false;
    return true;
}

constexpr std::array<uint8_t, 5> xboxone_s_init{0x05, 0x20, 0x00, 0x0f, 0x06};

// CDD NOTE the controllers I've tested with don't have a way to turn them back on once we've turned
// them off. Otherwise you have to press the guide button to wake them up - this behavior is
// consistent even if I plug them straight into the xbox
bool xboxh_power_on_controller(xbox_interface_t *p_itf) {
    if (p_itf->daddr == 0) return false;
    if (p_itf->is_powered) return false;
    const power_report_t power_on = make_power_report(0, std::to_underlying(power_mode_e::POWER_ON));

    TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, &power_on, sizeof(power_on)));
    wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);

    if ((p_itf->PID == kXboxOnePid4 || p_itf->PID == 0x0b00 || p_itf->PID == kXboxOnePid14)) {
        TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, xboxone_s_init.data(), sizeof(xboxone_s_init)));
        wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);
    }

    led_mode_command_t out =
            make_led_mode_command(get_sequence(), led_mode_e::LED_ON, 0x14);

    TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, &out, sizeof(out)));
    wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);

    p_itf->is_powered = true;
    return true;
}

bool xboxh_reset_controller(xbox_interface_t *p_itf) {
    xboxh_power_off_controller(p_itf);

    power_report_t out = make_power_report(get_sequence(), 0x07);

    TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, &out, sizeof(out)));
    wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);

    out.frame.sequence = get_sequence();
    out.data = 0x00;

    TU_ASSERT(xboxh_send_report(p_itf->daddr, 0, &out, sizeof(out)));
    wait_for_tx_complete(p_itf->daddr, p_itf->ep_out);

    TU_ASSERT(xboxh_power_on_controller(p_itf));
    return true;
}

// Consecutive failed interrupt-IN transfers. A wedged CH334R hub makes the
// controller's IN poll fail continuously (~80/s, result=FAILED) while a healthy-but-
// idle controller produces NO completions between its sparse packets (NAKs don't
// complete) -- so a run of failures is a fast, false-positive-free "wedged" signal
// (see host_recovery_task). Reset to 0 on any successful IN. Core1-only (xfer_cb runs
// in tuh_task) -> relaxed atomic, load/store only (no RMW): on M0+ this emits the same
// plain ldr/str as the prior volatile while documenting the cross-task publish intent.
constexpr auto kRlx = std::memory_order_relaxed;
std::atomic<uint32_t> s_in_err_streak{0};

}  // namespace

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

    memcpy(p_hid->epout_buf.data(), report, len);

    TU_LOG3_MEM(p_hid->epout_buf.data(), len, 2);

    if (!usbh_edpt_xfer(daddr, p_hid->ep_out, p_hid->epout_buf.data(), len)) {
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

    if (!usbh_edpt_xfer(daddr, p_controller->ep_in, p_controller->epin_buf.data(),
                        p_controller->epin_size)) {
        usbh_edpt_release(daddr, p_controller->ep_in);
        return false;
    }

    return true;
}

bool xboxh_init(void) {
    tu_memclr(_xbox_itf.data(), sizeof(_xbox_itf));
    return true;
}

bool xboxh_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf,
                uint16_t max_len) {
    (void)rhport;
    (void)max_len;

    // TU_LOG_USBH("Trying XBOX Controller with Interface %u\r\n", desc_itf->bInterfaceNumber);

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

    uint8_t const *p_desc = reinterpret_cast<uint8_t const *>(desc_itf);
    p_desc = tu_desc_next(p_desc);
    tusb_desc_endpoint_t const *desc_ep = reinterpret_cast<tusb_desc_endpoint_t const *>(p_desc);

    for (int i = 0; i < desc_itf->bNumEndpoints; i++) {
        TU_ASSERT(TUSB_DESC_ENDPOINT == desc_ep->bDescriptorType);
        TU_ASSERT(tuh_edpt_open(dev_addr, desc_ep));
        if (tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN) {
            uint16_t pkt_size = tu_edpt_packet_size(desc_ep);
            TU_ASSERT(pkt_size == XBOX_ONE_EP_MAXPKTSIZE);
            p_controller->ep_in = desc_ep->bEndpointAddress;
            p_controller->epin_size = pkt_size;
        } else {
            uint16_t pkt_size = tu_edpt_packet_size(desc_ep);
            TU_ASSERT(pkt_size == XBOX_ONE_EP_MAXPKTSIZE);
            p_controller->ep_out = desc_ep->bEndpointAddress;
            p_controller->epout_size = pkt_size;
        }

        p_desc = tu_desc_next(p_desc);
        desc_ep = reinterpret_cast<tusb_desc_endpoint_t const *>(p_desc);
    }
    p_controller->itf_num = desc_itf->bInterfaceNumber;
    p_controller->daddr = dev_addr;
    p_controller->PID = pid;
    p_controller->VID = vid;
    return true;
}

bool xboxh_set_config(uint8_t daddr, uint8_t itf_num) {
    TU_LOG_USBH("XBOXH Set Config addr: %02x interface: %d", daddr, itf_num);

    xbox_interface_t *p_hid = get_xbox_itf(daddr, itf_num);
    TU_VERIFY(p_hid);

    uint8_t idx = xbox_itf_get_index(daddr, itf_num);

    TU_ASSERT(xboxh_power_on_controller(p_hid));

    usbh_driver_set_config_complete(daddr, itf_num);

    if (xboxh_mount_cb) xboxh_mount_cb(daddr, idx);
    TU_ASSERT(xboxh_receive_report(daddr, idx));
    return true;
}

// Re-drive a mounted controller's power-on/init. An Xbox One GIP controller sends
// CMD_ANNOUNCE repeatedly after it (re)attaches until the host initialises it. Our
// one-shot power-on in xboxh_set_config() fires at mount, which a controller that
// fully re-powered its GIP layer announces *after* -- so it misses our init and then
// announces forever, never streaming input. Re-sending the init in response to an
// announce knocks it into the running state. Blocks on tx (wait_for_tx_complete pumps
// tuh_task), so call it from the core1 host loop -- never from inside a host callback.
bool xboxh_reinit_controller(uint8_t daddr, uint8_t idx) {
    xbox_interface_t *p_itf = get_xbox_itf(daddr, idx);
    TU_VERIFY(p_itf);
    p_itf->is_powered = false;  // force xboxh_power_on_controller() to re-send
    return xboxh_power_on_controller(p_itf);
}

void xboxh_power_off_controllers() {
    LOG_INFO(CAT_HOST, "Powering OFF Xbox Controllers");

    for (auto &itf : _xbox_itf) {
        xboxh_power_off_controller(&itf);
    }
}

void xboxh_power_on_controllers() {
    LOG_INFO(CAT_HOST, "Powering ON Xbox Controllers");

    for (auto &itf : _xbox_itf) {
        xboxh_power_on_controller(&itf);
    }
}

void xboxh_reset_controllers() {
    LOG_INFO(CAT_HOST, "RESETTING Xbox Controllers");

    for (auto &itf : _xbox_itf) {
        xboxh_reset_controller(&itf);
    }
}

uint32_t xboxh_in_error_streak(void) { return s_in_err_streak.load(kRlx); }
void xboxh_clear_error_streak(void) { s_in_err_streak.store(0, kRlx); }

bool xboxh_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    uint8_t const dir = tu_edpt_dir(ep_addr);

    if (dir == TUSB_DIR_IN) {
        if (result == XFER_RESULT_SUCCESS)
            s_in_err_streak.store(0, kRlx);
        else
            s_in_err_streak.store(s_in_err_streak.load(kRlx) + 1, kRlx);
    }

    uint8_t const idx = get_idx_by_epaddr(daddr, ep_addr);

    xbox_interface_t *p_controller = get_xbox_itf(daddr, idx);
    TU_VERIFY(p_controller);

    if (dir == TUSB_DIR_IN) {
        TU_LOG_USBH("  Get Report callback (%u, %u)\r\n", daddr, idx);
        p_controller->epin_buf.length = xferred_bytes;
        p_controller->epin_buf.triggered_time = orb::hal::Clock::duration{};
        p_controller->epin_buf.handled = 0;
        // TODO(rx-decouple): as with the device OUT path, prefer enqueuing epin_buf into a
        // bounded RX queue over this synchronous callback, so the controller-input consumer
        // isn't tied to tuh_task/core1. NOTE: this is the input HOT path -- a queue+task hop
        // adds latency, so measure before moving it (unlike the low-rate device control plane,
        // this one may justifiably stay inline on core1). See the "Fully decouple USB RX" task.
        if (xboxh_packet_received_cb)
            xboxh_packet_received_cb(idx, &p_controller->epin_buf, xferred_bytes);

        // xbox interface requires active polling

        // if(p_controller->epin_buf.frame.command == CMD_IDENTIFY &&
        // p_controller->epin_buf.frame.type & TYPE_ACK){
        //     xboxh_send_report(daddr, idx, )
        // }
        TU_ASSERT(xboxh_receive_report(daddr, idx));
    } else {
        p_controller->epout_buf.length = xferred_bytes;
        if (xboxh_packet_sent_cb)
            xboxh_packet_sent_cb(idx, &p_controller->epout_buf, xferred_bytes);
    }
    return true;
}

void xboxh_close(uint8_t daddr) {
    for (uint8_t i = 0; i < kXboxMaxControllers; i++) {
        xbox_interface_t *p_controller = &_xbox_itf[i];
        if (!p_controller) continue;
        if (p_controller->daddr == daddr) {
            p_controller->daddr = 0;
            p_controller->is_powered = false;
            if (xboxh_umount_cb) xboxh_umount_cb(daddr, i);
        }
    }
}
