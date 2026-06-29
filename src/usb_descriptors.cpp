/*
 * USB DEVICE descriptors (device descriptor, the configuration-descriptor tree, the
 * string table) and the three TinyUSB enumeration callbacks, as modern C++ behind the
 * unchanged extern "C" callback ABI TinyUSB calls by C symbol. Same C-facade pattern as
 * instrument_manager.cpp / wla_identifiers.cpp: the wire bytes are the USB ABI and stay
 * byte-for-byte identical to the original C (verified against the linked descriptor
 * symbols) -- any change breaks console enumeration -- while the C-isms around them are
 * modernised.
 *
 * Byte-identity is load-bearing:
 *  - the packed wire structs keep __attribute__((packed)) and their exact field layout;
 *  - desc_device / ConfigurationDescriptor keep their designated-initialiser values
 *    verbatim (C++20 designated init, already in declaration order);
 *  - the string descriptor is built into the same 128-word scratch buffer with the same
 *    length/format math.
 * What changed (touches no wire byte): the string-table index switch + UTF-16 conversion
 * loop are range-for over std::array/std::string_view; the scratch buffer is std::array;
 * the two never-referenced legacy structs (a duplicate device-descriptor struct and an
 * unused configuration-header struct) and the dead USB_CONFIG_ATTR_* / LANGUAGE_ID_ENG
 * macros were dropped (they emitted no bytes). The (char) -> uint16_t widening is now an
 * explicit static_cast<uint8_t>, which reproduces the original's ARM unsigned-char result
 * (e.g. the 0x90 in "MSFT100\x90" stays 0x0090) portably.
 *
 * tud_descriptor_*_cb stay extern "C" free functions and keep their exact return types
 * (uint8_t const* / uint16_t const*) -- TinyUSB resolves them by C symbol. This TU has no
 * header; nothing in our code calls these (only the USB stack does).
 */
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

// TinyUSB / adapter.h are plain C headers (adapter.h has no C-linkage seam of its own);
// wrap them in extern "C" so their symbols resolve to their C definitions -- same
// local-seam pattern instrument_manager.cpp / wla_identifiers.cpp use. The TinyUSB
// headers carry their own __cplusplus guards; the nesting is harmless.
extern "C" {
#include "bsp/board_api.h"
#include "adapter.h"
#include "common/tusb_types.h"
#include "device/usbd.h"
#include "tusb.h"
#include "tusb_option.h"
}

namespace {

constexpr std::uint16_t USB_VID = 0x0e6f;
constexpr std::uint16_t USB_PID = 0x0175;
constexpr std::uint16_t USB_BCD = 0x0200;

constexpr std::uint8_t NO_DESCRIPTOR = 0;
constexpr std::uint8_t USB_CONFIG_POWER_MA(unsigned mA) {
    return static_cast<std::uint8_t>(mA >> 1);
}

constexpr std::uint8_t ADAPTER_IN_NUM  = ENDPOINT_DIR_IN | 1;
constexpr std::uint8_t ADAPTER_OUT_NUM = ENDPOINT_DIR_OUT | 2;

// Endpoint bmAttributes is a 1-byte bitfield. The original C OR'd three distinct TinyUSB
// enum types directly; in C++20 enum|enum is deprecated, so fold them through an integral
// helper -- the truncated result byte is identical to the original expression.
constexpr std::uint8_t ep_attributes(unsigned xfer, unsigned sync, unsigned packet) {
    return static_cast<std::uint8_t>(xfer | sync | packet);
}

typedef struct {
    uint8_t Size; /**< Size of the descriptor, in bytes. */
    uint8_t Type; /**< Type of the descriptor, either a value in \ref USB_DescriptorTypes_t or a
                   * value given by the specific class.
                   */
} __attribute__((packed)) USB_Descriptor_Header_t;

typedef struct {
    USB_Descriptor_Header_t Header; /**< Descriptor header, including type and size. */

    uint8_t InterfaceNumber;  /**< Index of the interface in the current configuration. */
    uint8_t AlternateSetting; /**< Alternate setting for the interface number. The same
                               *   interface number can have multiple alternate settings
                               *   with different endpoint configurations, which can be
                               *   selected by the host.
                               */
    uint8_t TotalEndpoints;   /**< Total number of endpoints in the interface. */

    uint8_t Class;    /**< Interface class ID. */
    uint8_t SubClass; /**< Interface subclass ID. */
    uint8_t Protocol; /**< Interface protocol ID. */

    uint8_t InterfaceStrIndex; /**< Index of the string descriptor describing the interface. */
} __attribute__((packed)) USB_Descriptor_Interface_t;

typedef struct endpoint {
    USB_Descriptor_Header_t Header; /**< Descriptor header, including type and size. */

    uint8_t EndpointAddress; /**< Logical address of the endpoint within the device for the current
                              *   configuration, including direction mask.
                              */
    uint8_t Attributes;      /**< Endpoint attributes, comprised of a mask of the endpoint type
                              * (EP_TYPE_*)      and attributes (ENDPOINT_ATTR_*) masks.
                              */
    uint16_t EndpointSize;   /**< Size of the endpoint bank, in bytes. This indicates the maximum
                              * packet   size that the endpoint can receive at a time.
                              */
    uint8_t PollingIntervalMS; /**< Polling interval in milliseconds for the endpoint if it is an
                                * INTERRUPT or ISOCHRONOUS type.
                                */
} __attribute__((packed)) USB_Descriptor_Endpoint_t;

}  // namespace

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {.bLength            = sizeof(tusb_desc_device_t),
                                        .bDescriptorType    = TUSB_DESC_DEVICE,
                                        .bcdUSB             = USB_BCD,
                                        .bDeviceClass       = TUSB_CLASS_VENDOR_SPECIFIC,
                                        .bDeviceSubClass    = 0xFF,
                                        .bDeviceProtocol    = 0xFF,
                                        .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
                                        .idVendor           = USB_VID,
                                        .idProduct          = USB_PID,
                                        .bcdDevice          = 0x0200,
                                        .iManufacturer      = 0x01,
                                        .iProduct           = 0x02,
                                        .iSerialNumber      = 0x03,
                                        .bNumConfigurations = CFG_TUD_NUM_CONFIGURATIONS};

extern "C" uint8_t const *tud_descriptor_device_cb(void) {
    // printf("got device descriptor\r\n");
    return reinterpret_cast<uint8_t const *>(&desc_device);
}

//--------------------------------------------------------------------+
// Configuration Descriptor (tree)
//--------------------------------------------------------------------+
const struct {
    tusb_desc_configuration_t Config;

    USB_Descriptor_Interface_t Interface00;
    USB_Descriptor_Endpoint_t  I00ReportOUTEndpoint;
    USB_Descriptor_Endpoint_t  I00ReportINEndpoint;

    USB_Descriptor_Interface_t Interface10;
    USB_Descriptor_Interface_t Interface11;
    USB_Descriptor_Endpoint_t  I11ReportOUTEndpoint;
    USB_Descriptor_Endpoint_t  I11ReportINEndpoint;

    USB_Descriptor_Interface_t Interface20;
    USB_Descriptor_Interface_t Interface21;
    USB_Descriptor_Endpoint_t  I21ReportOUTEndpoint;
    USB_Descriptor_Endpoint_t  I21ReportINEndpoint;

} __attribute__((packed)) ConfigurationDescriptor = {
    .Config               = {.bLength             = sizeof(ConfigurationDescriptor.Config),
                             .bDescriptorType     = TUSB_DESC_CONFIGURATION,
                             .wTotalLength        = sizeof(ConfigurationDescriptor),
                             .bNumInterfaces      = 3,
                             .bConfigurationValue = 1,
                             .iConfiguration      = NO_DESCRIPTOR,
                             .bmAttributes        = (TU_BIT(7) | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP),
                             .bMaxPower           = USB_CONFIG_POWER_MA(500)},
    .Interface00          = {.Header            = {.Size = sizeof(ConfigurationDescriptor.Interface00),
                                                   .Type = TUSB_DESC_INTERFACE},
                             .InterfaceNumber   = 0x00,
                             .AlternateSetting  = 0x00,
                             .TotalEndpoints    = 2,
                             .Class             = 0xff,
                             .SubClass          = 0x47,
                             .Protocol          = 0xd0,
                             .InterfaceStrIndex = NO_DESCRIPTOR},
    .I00ReportOUTEndpoint = {.Header            = {.Size =
                                                       sizeof(ConfigurationDescriptor.I00ReportOUTEndpoint),
                                                   .Type = TUSB_DESC_ENDPOINT},
                             .EndpointAddress   = ADAPTER_OUT_NUM,
                             .Attributes        = ep_attributes(TUSB_XFER_INTERRUPT,
                                                                TUSB_ISO_EP_ATT_NO_SYNC,
                                                                TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 64,
                             .PollingIntervalMS = ADAPTER_OUT_INTERVAL},
    .I00ReportINEndpoint  = {.Header = {.Size = sizeof(ConfigurationDescriptor.I00ReportINEndpoint),
                                        .Type = TUSB_DESC_ENDPOINT},
                             .EndpointAddress   = ADAPTER_IN_NUM,
                             .Attributes        = ep_attributes(TUSB_XFER_INTERRUPT,
                                                                TUSB_ISO_EP_ATT_NO_SYNC,
                                                                TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 64,
                             .PollingIntervalMS = ADAPTER_IN_INTERVAL},

    .Interface10          = {.Header            = {.Size = sizeof(ConfigurationDescriptor.Interface10),
                                                   .Type = TUSB_DESC_INTERFACE},
                             .InterfaceNumber   = 0x01,
                             .AlternateSetting  = 0x00,
                             .TotalEndpoints    = 0,
                             .Class             = 0xff,
                             .SubClass          = 0xff,
                             .Protocol          = 0xd0,
                             .InterfaceStrIndex = NO_DESCRIPTOR},
    .Interface11          = {.Header            = {.Size = sizeof(ConfigurationDescriptor.Interface11),
                                                   .Type = TUSB_DESC_INTERFACE},
                             .InterfaceNumber   = 0x01,
                             .AlternateSetting  = 0x01,
                             .TotalEndpoints    = 2,
                             .Class             = 0xff,
                             .SubClass          = 0xff,
                             .Protocol          = 0xd0,
                             .InterfaceStrIndex = NO_DESCRIPTOR},
    .I11ReportOUTEndpoint = {.Header            = {.Size =
                                                       sizeof(ConfigurationDescriptor.I11ReportOUTEndpoint),
                                                   .Type = TUSB_DESC_ENDPOINT},
                             .EndpointAddress   = (ENDPOINT_DIR_OUT | 4),
                             .Attributes        = ep_attributes(TUSB_XFER_ISOCHRONOUS,
                                                                TUSB_ISO_EP_ATT_NO_SYNC,
                                                                TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 228,
                             .PollingIntervalMS = 0x01},
    .I11ReportINEndpoint  = {.Header = {.Size = sizeof(ConfigurationDescriptor.I11ReportINEndpoint),
                                        .Type = TUSB_DESC_ENDPOINT},
                             .EndpointAddress   = (ENDPOINT_DIR_IN | 3),
                             .Attributes        = ep_attributes(TUSB_XFER_ISOCHRONOUS,
                                                                TUSB_ISO_EP_ATT_NO_SYNC,
                                                                TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 228,
                             .PollingIntervalMS = 0x01},
    .Interface20          = {.Header            = {.Size = sizeof(ConfigurationDescriptor.Interface20),
                                                   .Type = TUSB_DESC_INTERFACE},
                             .InterfaceNumber   = 0x02,
                             .AlternateSetting  = 0x00,
                             .TotalEndpoints    = 0,
                             .Class             = 0xff,
                             .SubClass          = 0xff,
                             .Protocol          = 0xd0,
                             .InterfaceStrIndex = NO_DESCRIPTOR},
    .Interface21          = {.Header            = {.Size = sizeof(ConfigurationDescriptor.Interface21),
                                                   .Type = TUSB_DESC_INTERFACE},
                             .InterfaceNumber   = 0x02,
                             .AlternateSetting  = 0x01,
                             .TotalEndpoints    = 2,
                             .Class             = 0xff,
                             .SubClass          = 0xff,
                             .Protocol          = 0xd0,
                             .InterfaceStrIndex = NO_DESCRIPTOR},
    .I21ReportOUTEndpoint = {.Header          = {.Size =
                                                     sizeof(ConfigurationDescriptor.I21ReportOUTEndpoint),
                                                 .Type = TUSB_DESC_ENDPOINT},
                             .EndpointAddress = (ENDPOINT_DIR_OUT | 6),
                             .Attributes      = ep_attributes(TUSB_XFER_BULK, TUSB_ISO_EP_ATT_NO_SYNC,
                                                              TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 64,
                             .PollingIntervalMS = 0x00},
    .I21ReportINEndpoint  = {.Header = {.Size = sizeof(ConfigurationDescriptor.I21ReportINEndpoint),
                                        .Type = TUSB_DESC_ENDPOINT},
                             .EndpointAddress = (ENDPOINT_DIR_IN | 5),
                             .Attributes      = ep_attributes(TUSB_XFER_BULK, TUSB_ISO_EP_ATT_NO_SYNC,
                                                              TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 64,
                             .PollingIntervalMS = 0x00}

};

extern "C" uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;  // for multiple configurations
    return reinterpret_cast<uint8_t const *>(&ConfigurationDescriptor);
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
namespace {

// Index 0 is the language-ID descriptor (0x0409, little-endian as two bytes), not text;
// the remaining entries are the manufacturer/product/serial/security strings plus the
// MS-OS string (index 5, returned for the 0xEE request).
constexpr char language_id[] = {0x09, 0x04};
constexpr std::array<const char *, 6> string_desc_arr = {
    language_id,
    "Performance Designed Products",
    "Rock Band Wired Legacy Adapter for Xbox One",
    "0000079C605C69B6",
    "Xbox Security Method 3, Version 1.00, © 2005 Microsoft Corporation. All rights reserved.",
    "MSFT100\x90",
};

// Scratch buffer the callback returns a view into; persists across the call (static).
std::array<std::uint16_t, 128> desc_str{};

}  // namespace

extern "C" uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count;

    if (index == 0) {
        std::memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index == 0xEE) {
            index = 5;  // MS-OS string descriptor
        } else if (index > 4) {
            return nullptr;
        }

        // Cap at the max char count the scratch buffer (127 chars + length word) holds.
        std::string_view str{string_desc_arr[index]};
        chr_count = static_cast<uint8_t>(str.size() > 127 ? 127 : str.size());

        std::size_t i = 0;
        for (char c : str.substr(0, chr_count)) {
            // Explicit uint8_t widening reproduces the original's ARM unsigned-char
            // result (e.g. 0x90 -> 0x0090), independent of char signedness.
            desc_str[1 + i++] = static_cast<std::uint8_t>(c);
        }
    }

    desc_str[0] = static_cast<std::uint16_t>((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str.data();
}
