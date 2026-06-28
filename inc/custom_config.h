/*
    The MIT License (MIT)

    Copyright (c) 2018, hathach for Adafruit

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
*/

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// RHPort max operational speed can defined by board.mk
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED OPT_MODE_DEFAULT_SPEED
#endif

// Default is max speed that hardware controller could support with on-chip PHY
#define CFG_TUD_MAX_SPEED BOARD_TUD_MAX_SPEED

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_HOST)

// Enable device stack
#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 1

#define CFG_TUH_RPI_PIO_USB 1
#define BOARD_TUH_RHPORT CFG_TUH_RPI_PIO_USB

#define CFG_TUSB_MCU OPT_MCU_RP2040
// The vendored tinyusb rp2040 BSP (hw/bsp/rp2040/family.cmake) hard-defines
// -DCFG_TUSB_OS=OPT_OS_PICO on the command line. This config file is included by
// tusb_option.h before the OSAL is selected, so undef + redefine here overrides it
// cleanly (no redefinition warning) and consistently across every TinyUSB unit.
#undef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_FREERTOS

// temporary: route host-stack logs through the deferred (core1-safe) logger
int dlog_printf(const char *fmt, ...);
#define CFG_TUSB_DEBUG 0
#define CFG_TUSB_DEBUG_PRINTF dlog_printf

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN TU_ATTR_ALIGNED(4)

//--------------------------------------------------------------------
// Device Configuration
//--------------------------------------------------------------------
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUD_NUM_CONFIGURATIONS 1

#define CFG_TUD_XINPUT 1
#define CFG_TUD_XINPUT_TX_BUFSIZE 64
#define CFG_TUD_XINPUT_RX_BUFSIZE 64

#define CFG_TUH_XINPUT 1

#define CFG_TUH_DEVICE_MAX (4)

#define CFG_TUH_HID 2
#define CFG_TUH_HUB 1

// Mass storage HOST: lets us log to a USB flash drive plugged into the hub
// (src/usb_log.c, via FatFs). Built-in TinyUSB host driver, auto-registered.
#define CFG_TUH_MSC 1

//--------------------------------------------------------------------
// Host Configuration
//--------------------------------------------------------------------

// Size of buffer to hold descriptors and other data used for enumeration
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// Number of hub devices

#define CFG_TUD_LOG_LEVEL 0
#define CFG_TUH_LOG_LEVEL 2

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
