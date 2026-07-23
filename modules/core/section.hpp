/*
 * core/section.hpp — portable linker-section placement macros (ZERO deps; NO SDK include).
 *
 * ORB_FAST(name)  — place a function in RAM so it never incurs a flash-fetch stall. Used on
 *                   the core1 PIO-USB hot path and the deferred-log/MIDI fast paths.
 * ORB_FLASH       — force a `const` aggregate to stay in flash (never copied to RAM).
 *
 * On the Pico platform these reproduce the SDK's __not_in_flash_func()/__in_flash() section
 * attributes BYTE-FOR-BYTE (RAM ".time_critical.<name>" / flash ".flashdata."), so adopting
 * them changes no codegen. Off-platform (host unit tests) they are no-ops.
 *
 * This header is deliberately self-contained: it does NOT include any pico-sdk header, so it
 * is safe to include from the portable logic layers (service/protocol/log/app) without
 * dragging the SDK across the portability boundary. It keys off PICO_ON_DEVICE — the compile
 * definition the pico_platform target already injects onto every TU (-DPICO_ON_DEVICE=1) —
 * rather than relying on an SDK macro being transitively in scope.
 *
 * Porting: a new platform that wants RAM/flash placement adds its own attribute here behind
 * its platform define; everything above stays unchanged.
 */
#pragma once

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
// Matches pico-sdk pico/platform/sections.h:
//   __not_in_flash_func(f) -> __attribute__((section(".time_critical." #f))) f
//   __in_flash()           -> __attribute__((section(".flashdata.")))
#define ORB_FAST(name) __attribute__((section(".time_critical." #name))) name
#define ORB_FLASH __attribute__((section(".flashdata.")))
#else
#define ORB_FAST(name) name
#define ORB_FLASH
#endif
