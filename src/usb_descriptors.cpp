#include "bsp/board_api.h"
// #include "bsp/board.h"
#include "adapter.h"
#include "common/tusb_types.h"
#include "device/usbd.h"
#include "tusb.h"
#include "tusb_option.h"

#define USB_VID 0x0e6f
#define USB_PID 0x0175
#define USB_BCD 0x0200

#define NO_DESCRIPTOR 0
#define USB_CONFIG_POWER_MA(mA) ((mA) >> 1)

#define USB_CONFIG_ATTR_REMOTEWAKEUP 0x20
#define USB_CONFIG_ATTR_RESERVED 0x80

#define ADAPTER_IN_NUM (ENDPOINT_DIR_IN | 1)
#define ADAPTER_OUT_NUM (ENDPOINT_DIR_OUT | 2)

#define LANGUAGE_ID_ENG 0x0409

typedef struct {
    uint8_t Size; /**< Size of the descriptor, in bytes. */
    uint8_t Type; /**< Type of the descriptor, either a value in \ref USB_DescriptorTypes_t or a
                   * value given by the specific class.
                   */
} __attribute__((packed)) USB_Descriptor_Header_t;

typedef struct {
    USB_Descriptor_Header_t Header; /**< Descriptor header, including type and size. */

    uint16_t TotalConfigurationSize; /**< Size of the configuration descriptor header,
                                      *   and all sub descriptors inside the configuration.
                                      */
    uint8_t TotalInterfaces;         /**< Total number of interfaces in the configuration. */

    uint8_t ConfigurationNumber; /**< Configuration index of the current configuration. */
    uint8_t
        ConfigurationStrIndex; /**< Index of a string descriptor describing the configuration. */

    uint8_t ConfigAttributes; /**< Configuration attributes, comprised of a mask of \c
                               * USB_CONFIG_ATTR_* masks. On all devices, this should include
                               * USB_CONFIG_ATTR_RESERVED at a minimum.
                               */

    uint8_t MaxPowerConsumption; /**< Maximum power consumption of the device while in the
                                  *   current configuration, calculated by the \ref
                                  * USB_CONFIG_POWER_MA() macro.
                                  */
} __attribute__((packed)) USB_Descriptor_Configuration_Header_t;

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

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
/* Device descriptor structure */
typedef struct tes {
    uint8_t  bLength;             // Length of this descriptor.
    uint8_t  bDescriptorType;     // DEVICE descriptor type (USB_DESCRIPTOR_DEVICE).
    uint16_t bcdUSB;              // USB Spec Release Number (BCD).
    uint8_t  bDeviceClass;        // Class code (assigned by the USB-IF). 0xFF-Vendor specific.
    uint8_t  bDeviceSubClass;     // Subclass code (assigned by the USB-IF).
    uint8_t  bDeviceProtocol;     // Protocol code (assigned by the USB-IF). 0xFF-Vendor specific.
    uint8_t  bMaxPacketSize0;     // Maximum packet size for endpoint 0.
    uint16_t idVendor;            // Vendor ID (assigned by the USB-IF).
    uint16_t idProduct;           // Product ID (assigned by the manufacturer).
    uint16_t bcdDevice;           // Device release number (BCD).
    uint8_t  iManufacturer;       // Index of String Descriptor describing the manufacturer.
    uint8_t  iProduct;            // Index of String Descriptor describing the product.
    uint8_t  iSerialNumber;       // Index of String Descriptor with the device's serial number.
    uint8_t  bNumConfigurations;  // Number of possible configurations.
} __attribute__((packed)) USB_DEVICE_DESCRIPTOR1;

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

uint8_t const *tud_descriptor_device_cb(void) {
    // printf("got device descriptor\r\n");
    return (uint8_t const *)&desc_device;
}

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
                             .Attributes        = (TUSB_XFER_INTERRUPT | TUSB_ISO_EP_ATT_NO_SYNC |
                                            TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 64,
                             .PollingIntervalMS = ADAPTER_OUT_INTERVAL},
    .I00ReportINEndpoint  = {.Header = {.Size = sizeof(ConfigurationDescriptor.I00ReportINEndpoint),
                                        .Type = TUSB_DESC_ENDPOINT},
                             .EndpointAddress   = ADAPTER_IN_NUM,
                             .Attributes        = (TUSB_XFER_INTERRUPT | TUSB_ISO_EP_ATT_NO_SYNC |
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
                             .Attributes        = (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_NO_SYNC |
                                            TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 228,
                             .PollingIntervalMS = 0x01},
    .I11ReportINEndpoint  = {.Header = {.Size = sizeof(ConfigurationDescriptor.I11ReportINEndpoint),
                                        .Type = TUSB_DESC_ENDPOINT},
                             .EndpointAddress   = (ENDPOINT_DIR_IN | 3),
                             .Attributes        = (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_NO_SYNC |
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
                             .Attributes =
                                 (TUSB_XFER_BULK | TUSB_ISO_EP_ATT_NO_SYNC | TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 64,
                             .PollingIntervalMS = 0x00},
    .I21ReportINEndpoint  = {.Header = {.Size = sizeof(ConfigurationDescriptor.I21ReportINEndpoint),
                                        .Type = TUSB_DESC_ENDPOINT},
                             .EndpointAddress = (ENDPOINT_DIR_IN | 5),
                             .Attributes =
                                 (TUSB_XFER_BULK | TUSB_ISO_EP_ATT_NO_SYNC | TUSB_ISO_EP_ATT_DATA),
                             .EndpointSize      = 64,
                             .PollingIntervalMS = 0x00}

};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;  // for multiple configurations
    return (uint8_t const *)&ConfigurationDescriptor;
}

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Performance Designed Products",
    "Rock Band Wired Legacy Adapter for Xbox One",
    "0000079C605C69B6",
    "Xbox Security Method 3, Version 1.00, © 2005 Microsoft Corporation. All rights reserved.",
    "MSFT100\x90"};

static uint16_t _desc_str[128];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count;
    switch (index) {
        case 0:
            memcpy(&_desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;
        case 0xEE:
            index = 5;
            // fallthrough
        case 1:
        case 2:
        case 3:
        case 4: {
            const char *str = string_desc_arr[index];
            // Cap at max char
            chr_count = (uint8_t)strlen(str);
            if (chr_count > 127)
                chr_count = 127;

            for (uint8_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = str[i];
            }
            break;
        }
        default:
            return NULL;
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}