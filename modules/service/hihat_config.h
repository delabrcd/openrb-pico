#pragma once

// Compile-time configuration for the alternate (CC-keyed) hi-hat mode.
//
// See docs/features/hihat-mode.md for the full spec. This header is the
// PHASE-1 COMPILE-TIME STOPGAP for the tunables: the spec's end state stores
// these in the runtime configurator (docs/features/runtime-configurator.md,
// still PROPOSED). Until that lands, the values below are baked in at build
// time. When the runtime configurator ships, these #defines become its
// defaults and the live values are read from persisted settings instead.
//
// EVERYTHING the feature adds is gated on ORB_HIHAT_MODE so the DEFAULT build
// (mode 0) is byte-for-byte identical to the pure note-relay firmware.

// --- Feature gate ------------------------------------------------------------
// 0 = DEFAULT: feature entirely compiled out (pure relay; byte-identical build).
// 1 = CC parsing + discovery logging ON, but NO output override. Relay behavior
//     is preserved. Use this to watch the UART and learn which CC# your kit
//     sends for the hi-hat pedal and its polarity (spec Phase 0).
// 2 = everything in 1 PLUS the CC-keyed override: the g_hh_open threshold/
//     hysteresis state machine and the note_on() open/closed override.
#ifndef ORB_HIHAT_MODE
#define ORB_HIHAT_MODE 0
#endif

// --- Tunables (active at ORB_HIHAT_MODE >= 2) --------------------------------

// Pedal-position controller number to watch. CC#4 ("Foot Controller") is the
// most common hi-hat-pedal CC on Roland TD / Yamaha DTX modules, but it is
// kit-specific -- use mode 1 to discover the real one for your kit.
#ifndef ORB_HIHAT_CC
#define ORB_HIHAT_CC 4
#endif

// Open/closed split point on the 0..127 pedal-position range. "Raw closeness"
// (see ORB_HIHAT_INVERT) at or above the threshold means closed.
#ifndef ORB_HIHAT_THRESHOLD
#define ORB_HIHAT_THRESHOLD 64
#endif

// Hysteresis band around the threshold. Prevents open/closed chatter when the
// pedal hovers near the split point: the state only flips once the pedal moves
// past threshold +/- this much.
#ifndef ORB_HIHAT_HYST
#define ORB_HIHAT_HYST 8
#endif

// CC polarity. 0 = a LOW CC value means open (pedal up), HIGH means closed
// (pedal down) -- the spec's assumed default. Set to 1 to flip it for kits
// where a high value means open.
#ifndef ORB_HIHAT_INVERT
#define ORB_HIHAT_INVERT 0
#endif

// Hi-hat strike note set. A strike on any of these notes has its open/closed
// output overridden by the tracked pedal state (mode 2). Defaults to the GM +
// Roland-edge hi-hat notes: 22 (closed edge), 26 (open edge), 42 (GM closed),
// 46 (GM open). Note 44 (pedal chick) is intentionally NOT included -- it is a
// pedal sound, not a stick strike (see spec "Pedal note (note 44)").
#ifndef ORB_HIHAT_NOTES
#define ORB_HIHAT_NOTES \
    { 22, 26, 42, 46 }
#endif

