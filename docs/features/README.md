# Feature specs

Design docs / specs for openrb-pico features; they capture the intent, a code-grounded
design, a phased plan, and the open decisions for each. They cross-reference each other
and the port handoff ([`../FREERTOS-PORT.md`](../FREERTOS-PORT.md)). Implementation status
is tracked per spec below and in each spec's `Status:` line.

| Spec | What | Status |
|---|---|---|
| [cpp-overhaul.md](cpp-overhaul.md) | Object-oriented overhaul: RAII C++ wrappers over the FreeRTOS/TinyUSB C APIs, no-heap fixed-capacity containers, module-by-module migration. | **In progress** — foundations (`Result`/`SpscRing`/`StaticTask`/`StaticQueue`/`SoftwareTimer`/`Mutex`, all `namespace orb`), `-fno-exceptions/-rtti`, dlog+usb_log rings → `SpscRing`, app_queues+midi onto wrappers, packet builders + size `static_assert` done & reviewed. Remaining: hardware RAII, `adapter_ctx` object, device/host driver classes, task/UsbHost objects (core1/driver — need hardware validation). |
| [logging.md](logging.md) | Unified deferred logger: levels, category tags, timestamps, `[ts][LEVEL][core][CAT]` format over the existing core-safe SPSC rings. | **Implemented** — front end + all call sites migrated; plain-text output with host-side colorization (`scripts/uart.sh`). Phase 5 per-sink/per-category *runtime* level control remains optional. |
| [runtime-configurator.md](runtime-configurator.md) | UI-over-USB configurator (Santroller-style): versioned config (timing + MIDI mappings + hi-hat mode) read/written over USB and persisted to flash around the QSPI/PIO-USB hazard. | Proposed — not started (high-risk: flash + EP0 vendor transfers + multicore lockout; needs design sign-off). |
| [hihat-mode.md](hihat-mode.md) | Alternate hi-hat mode keyed off the MIDI pedal Control Change (open/closed) instead of relaying kit notes. | **Implemented (gated)** — behind compile-time `ORB_HIHAT_MODE` (default 0 = off, byte-identical build; 1 = CC discovery log; 2 = CC-keyed override). Phase 2 (runtime-config-backed tunables) pends the configurator. Needs bench tuning of CC#/polarity. |

These are independent but related: the **configurator** persists the tunables the
**hi-hat mode** and timing constants need; the **logging** overhaul and **C++ overhaul**
are cross-cutting and ideally land (at least partly) before the larger features build on
them.
