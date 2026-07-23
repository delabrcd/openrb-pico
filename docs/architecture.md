# Architecture

openrb-pico is an RP2040 firmware that reads a rhythm-game instrument — a drum kit over
**USB MIDI** (the PIO-USB host) or **serial MIDI** (a UART) — and presents to a console as
an Xbox One pro-drums controller. **Guitar (HID) support is in progress.** It runs a
**FreeRTOS-SMP** build across both cores: **core1** runs the single bit-banged PIO-USB
host task; **core0** runs the USB-device stack and all feature logic. The chip runs at
**120 MHz** ([`modules/app/main.cpp`](../modules/app/main.cpp) `set_sys_clock_khz(120000)`).

This doc is the authoritative description of the layered C++23 design and the runtime
model. The enforceable *rules* contributors must follow live in
[`AGENTS.md`](../AGENTS.md); this is the *why* and the *shape*.

## Layered modules

The code is organized bottom-up under [`modules/`](../modules). Each layer is a CMake
`INTERFACE`/object library that links **only lower layers** — the link graph in
[`CMakeLists.txt`](../CMakeLists.txt) *is* the architecture, and the boundary is enforced
in CI (see below).

| Layer | Depends on | What it is |
|-------|-----------|------------|
| `core` | — | Portable, zero-dependency utilities: `Result`, `SpscRing`, `static_vector`, `function_ref`, `lifetime` (`start_lifetime_as`). Header-only, no SDK, no RTOS. |
| `osal` | core | RTOS abstraction over FreeRTOS: `Task<N>`, `Queue<T,N>`, `Mutex`, `Timer`, `critical`, tick `chrono`. Owns `FreeRTOSConfig.h`. |
| `hal` | platform | Compile-time hardware interface (concepts + value types): `GpioOut`, `Uart`, `Clock`, `IrqGuard`. Zero-cost — no vtables on hot paths. |
| `platform/pico` | (sdk) | The concrete pico-sdk implementation of the HAL. The **only** layer, besides the driver seam, meant to touch pico-sdk. |
| `log` | core, hal | Deferred logging: `dlog` rings, `orb_log` front end, `usb_log` USB-stick sink, the `orb_debug.h` gate. |
| `protocol` | core, log | Wire logic: the Xbox One protocol, WLA identifiers. No hardware. |
| `service` | core, protocol, osal, log, board | Hardware-free feature logic: `DrumEngine` (fed by both USB-MIDI and serial-MIDI), `SerialMidi`, `InstrumentManager`, and `Guitar` (HID, in progress). **No SDK/RTOS/TinyUSB includes** (CI-enforced). |
| `driver` | core, protocol, service, log, hal | The vendor/TinyUSB seam — Xbox host/device drivers, MIDI seam, descriptors, PIO-USB glue. The sanctioned place for `extern "C"` and TinyUSB includes. |
| `board` | (pins) | Board pin maps + GPIO actuation (`Actuators`: LED, hub RESET#, 5 V enable). |
| `app` | everything | Orchestration / composition root: `main`, `system` (wires the object graph), `app_tasks`, `device_session`, `host_controller`, `housekeeping`, `recovery`. |

## Design principles

- **Static dependency injection, one composition root.** The whole object graph is
  constructed once in [`modules/app/system.cpp`](../modules/app/system.cpp) and passed by
  reference; there are no singletons or free-function forwarders. A CI guard
  ([`scripts/check-boundary.sh`](../scripts/check-boundary.sh)) fails the build if the
  deleted forwarders reappear anywhere in `modules/`.
- **No heap.** No `new`/`malloc` in firmware paths; tasks, queues, and buffers are static
  `orb::osal` wrappers and `constinit` state.
- **The portability boundary is enforced, not aspirational.** `check-boundary.sh` greps
  `modules/service/` and `modules/protocol/` for forbidden SDK/RTOS/TinyUSB includes and
  bare FreeRTOS types (`TaskHandle_t`, …). Only `driver/` and `platform/` may touch the
  vendor stacks.
- **`void*` is confined** to a small CI allowlist (the `osal` task/timer trampolines,
  `function_ref`, `lifetime`, and the vendor seams); anywhere else it fails CI.
- **The HAL is zero-cost.** Hardware access goes through compile-time concepts and value
  types, so the abstraction costs nothing at `-O2` — no vtable indirection on the USB or
  drum hot paths.

## Runtime model (FreeRTOS SMP)

`configNUMBER_OF_CORES=2`, core affinity on, `configMAX_PRIORITIES=8`, 1 kHz tick,
`configCPU_CLOCK_HZ=120000000` ([`modules/osal/FreeRTOSConfig.h`](../modules/osal/FreeRTOSConfig.h)).
The scheduler itself launches core1 (`vTaskStartScheduler()` in `main.cpp`) — the firmware
never calls `multicore_launch_core1`. All tasks are created in one place,
[`modules/app/app_tasks.cpp`](../modules/app/app_tasks.cpp):

| Task | Core | Prio | Role |
|------|:----:|:----:|------|
| `usb_host` | 1 | 6 | The **only** core1 task: PIO-USB host loop (`tuh_task`, host-tx drain, MIDI read, USB-stick writes, runtime recovery). [`host_controller.cpp`](../modules/app/host_controller.cpp) |
| `usb_dev` | 0 | 6 | Xbox One device stack + device-tx drain. [`device_session.cpp`](../modules/app/device_session.cpp) |
| `drum_in` | 0 | 5 | Drain MIDI notes + serial MIDI, age hits, emit drum packets. [`drums.cpp`](../modules/service/drums.cpp) |
| `instr` | 0 | 5 | Drain the instrument hot-plug event queue. [`instrument_manager.cpp`](../modules/service/instrument_manager.cpp) |
| `housekeep` | 0 | 4 | Announce cadence, reboot-recovery service, log drain. [`housekeeping.cpp`](../modules/app/housekeeping.cpp) |

**Inter-core communication** is all lock-free rings or `osal` queues — never a cross-core
spinlock:

- `midi_notes` — core1 → core0 (`Queue<midi_note_t, 32>`): parsed host-MIDI notes.
- `host_tx` — core0 → core1 (`osal::Queue`): device-side packets to relay to the host.
- `device_tx` — both cores → `usb_dev` (`tu_fifo` + a write mutex): outbound Xbox packets.
- `adapter` context — both cores: aligned lock-free volatiles (adapter state, controller
  address, alive/seen flags).
- `dlog` / `usb_log` rings — SPSC, drained on the owning core (see [Logging](https://github.com/delabrcd/openrb-pico/wiki/Logging) on the wiki).

## core1 concurrency rules

core1 owns timing-critical PIO-USB. It runs **only** `usb_host`, and the following are
forbidden on it (they take spinlocks or yield and will wedge the bus): `sleep_ms` /
`sleep_us`, `board_millis`, blocking or unbounded mutexes, and any non-relaxed
`std::atomic` (this is an M0+; only relaxed ordering is free). Pre-scheduler code must use
`busy_wait_ms`, not `sleep_ms` — a `sleep` before the scheduler is up deadlocks the boot.
These rules are restated as hard requirements in [`AGENTS.md`](../AGENTS.md).

## Build-time feature gates

Four options select the feature/logging profile at compile time; they propagate through the
`orb_buildconfig` interface lib to every board target. See
[`BUILDING.md`](../BUILDING.md) and [`CMakePresets.json`](../CMakePresets.json).

| Option | Default | Effect |
|--------|:-------:|--------|
| `ORB_DEBUG` | ON | OFF drops all `LOG_*` + magic_enum reflection (release image). |
| `ORB_LOG_LEVEL` | DEBUG | Compile-time log floor (NONE…TRACE). |
| `ORB_LOG_COLOR` | OFF | Bake ANSI color into the UART stream. |
| `ORB_HIHAT_MODE` | 0 | CC-keyed hi-hat mode (see [Hi-Hat Mode](https://github.com/delabrcd/openrb-pico/wiki/Hi-Hat-Mode) on the wiki). |

The `release` preset is `ORB_DEBUG=OFF` + `ORB_LOG_LEVEL=WARN`. An orthogonal
`ORB_DEBUG_BUILD=ON` compiles the project's own sources at `-Og` with full unwind info for
deep gdb backtraces (see [`docs/DEBUGGING.md`](DEBUGGING.md)).

## Design history

How the host stack, the 240 → 120 MHz clock decision, and the FreeRTOS-SMP move came to be
is recorded on the wiki: [Design History](https://github.com/delabrcd/openrb-pico/wiki/Design-History).
