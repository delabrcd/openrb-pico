#ifndef ORB_BSP_H_
#define ORB_BSP_H_
#define ORB_IN_BSP

#define ORB_BOARD_ID_FEATHER 1
#define ORB_BOARD_ID_CUSTOM_REV_0_1 2
#define ORB_BOARD_ID_CUSTOM_REV_0_2 3

#ifndef ORB_BOARD_ID
#pragma GCC error "ORB_BOARD_ID must be set"
#endif

#if ORB_BOARD_ID == ORB_BOARD_ID_FEATHER
#include "pins_rp2040_usbh.h"  // IWYU pragma: keep
#elif ORB_BOARD_ID == ORB_BOARD_ID_CUSTOM_REV_0_1
#include "pins_rp2040_orb_custom_0_1.h"  // IWYU pragma: keep
#else
#pragma GCC error "Unsupported Board ID"
#endif

#undef ORB_IN_BSP

#endif  // ORB_BSP_H_