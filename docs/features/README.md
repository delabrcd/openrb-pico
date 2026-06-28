# Feature specs

Design docs / specs for planned openrb-pico features. Each is **PROPOSED** (spec only —
not yet implemented); they capture the intent, a code-grounded design, a phased plan,
and the open decisions for each feature so implementation can start from an agreed shape.
They cross-reference each other and the port handoff ([`../FREERTOS-PORT.md`](../FREERTOS-PORT.md)).

| Spec | What | Status |
|---|---|---|
| [cpp-overhaul.md](cpp-overhaul.md) | Object-oriented overhaul: RAII C++ wrappers over the FreeRTOS/TinyUSB C APIs, no-heap fixed-capacity containers, module-by-module migration. | Proposed |
| [logging.md](logging.md) | Unified deferred logger: levels, category tags, timestamps, professional `[ts][LEVEL][core][CAT]` format over the existing core-safe SPSC rings. | Proposed |
| [runtime-configurator.md](runtime-configurator.md) | UI-over-USB configurator (Santroller-style): versioned config (timing + MIDI mappings + hi-hat mode) read/written over USB and persisted to flash around the QSPI/PIO-USB hazard. | Proposed |
| [hihat-mode.md](hihat-mode.md) | Alternate hi-hat mode keyed off the MIDI pedal Control Change (open/closed) instead of relaying kit notes. | Proposed |

These are independent but related: the **configurator** persists the tunables the
**hi-hat mode** and timing constants need; the **logging** overhaul and **C++ overhaul**
are cross-cutting and ideally land (at least partly) before the larger features build on
them.
