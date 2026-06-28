/*
 * orb_c_api.h — the single, permanent C/C++ boundary seam (see
 * docs/features/cpp-overhaul.md D2).
 *
 * TinyUSB, FreeRTOS, the Pico SDK and FatFs call INTO this firmware through a fixed set
 * of C-linkage symbols (tu*_cb / usbd_app_driver_get_cb / disk_* / FreeRTOS hooks /
 * tusb_time_delay_ms_api / the TaskFunction_t task entries). Those symbols must NEVER be
 * name-mangled, become member functions, or change signature. As C++ objects take over
 * the stateful internals, each such symbol stays a thin `extern "C"` shim that forwards
 * into a C++ object — this header is where that convention is spelled out and where the
 * forwarding declarations live.
 *
 * Use ORB_C_BEGIN / ORB_C_END around blocks of C-linkage declarations in headers that
 * are included from both C and C++ TUs (equivalent to the hand-written
 * `#ifdef __cplusplus extern "C" {` guards already used across inc/). ORB_C_API marks a
 * single declaration.
 *
 *   ORB_C_BEGIN
 *   void xboxh_xfer_cb(...);   // implemented as an extern "C" shim -> XboxController
 *   ORB_C_END
 */
#ifndef OPENRB_ORB_C_API_H
#define OPENRB_ORB_C_API_H

#ifdef __cplusplus
#define ORB_C_API extern "C"
#define ORB_C_BEGIN extern "C" {
#define ORB_C_END }
#else
#define ORB_C_API
#define ORB_C_BEGIN
#define ORB_C_END
#endif

#endif  // OPENRB_ORB_C_API_H
