# OpenRB-pico — modern-C++ rearchitecture

Status: IN PROGRESS. This is the authoritative architecture for the full rewrite of the
firmware into layered, idiomatic, **portable** modern C++ (C++20). It supersedes the
narrower [`../features/cpp-overhaul.md`](../features/cpp-overhaul.md) (which covered the
first wrapper/foundation steps — those remain valid and are folded in here).

## The goal, stated as a test

> *How hard would it be to port this firmware to a different microcontroller?*

Today: very hard — hardware access (Pico SDK, registers, FreeRTOS, TinyUSB) is threaded
directly through the feature logic in C. Target: **swap one directory** (`platform/pico/`)
and the app builds for another MCU. That forces the discipline we want anyway: the logic
layer becomes hardware-free, testable on a host, and written in modern C++.

## Principles (non-negotiable)

1. **Layered, dependencies point downward.** A layer may use the layers below it and the
   pure `core/` utilities — never a layer above, never sideways into another platform.
2. **Only `platform/**` touches the SDK / RTOS / chip.** No `#include "pico/..."`,
   `"hardware/..."`, `<FreeRTOS.h>`, `task.h`, or TinyUSB headers anywhere in
   `core/`, `service/`, or the *interfaces* of `hal/`/`osal/`. Enforced by a CI grep
   (P6) — see [Portability boundary](#portability-boundary).
3. **No heap. Ever.** Static/inline storage only — the core1 PIO-USB timing forbids a
   hidden `malloc`/lock, and we want determinism. This shapes every type choice below.
4. **Modern C++ idiom is the default, C-isms are bugs.** No C arrays (`std::array`), no
   raw owning pointers (values / references / handles), no `(ptr, len)` pairs
   (`std::span`), no index-for where a range-for or algorithm fits, no function pointers
   in app code (non-owning `function_ref` / owning `inplace_function`), `enum class`,
   RAII, `[[nodiscard]]`, `constexpr`/`constinit` where it buys safety.
5. **The C seam is thin and one-directional.** TinyUSB/FreeRTOS call *into* us through a
   fixed set of `extern "C"` symbols (`tu*_cb`, task entries, diskio, hooks). Those stay
   C-linkage shims that immediately forward into a C++ object. We never expose a C API
   *upward* into the logic.
6. **Zero runtime cost for the abstraction.** The HAL is compile-time-bound (concepts +
   a platform-selected concrete type), not virtual — one MCU per build, so a vtable
   indirection on the SOF/endpoint hot path buys nothing. Runtime polymorphism only
   where genuinely needed.

## Layers

```
            ┌────────────────────────────────────────────────────────────┐
  app/      │ orchestration: main, tasks, the UsbHost/Adapter wiring.     │
            │ Knows the whole system; owns object lifetimes (static).     │
            └───────────────┬────────────────────────────────────────────┘
                            ▼
  driver/   │ device classes that bind a protocol to hardware via HAL:    │
            │ XboxDevice, XboxHostController, PioUsbHost, SerialMidi,      │
            │ MscLogSink. Live behind the extern "C" TinyUSB seam.        │
                            ▼
  service/  │ feature logic, HARDWARE-FREE & portable & host-testable:    │
            │ DrumEngine, Guitar, InstrumentManager, AdapterState,        │
            │ xbox protocol (builders/parsers), MidiParser, HiHat, log    │
            │ policy. Depends only on core/ + osal/ + hal/ *interfaces*.  │
                            ▼
  hal/      │ interface (concepts + small value types): GpioOut, Uart,    │
  osal/     │ Clock, Timer, Dma, Irq/CriticalSection, UsbPhy, Flash;      │
            │ osal: Task, Queue, Mutex, SwTimer, Notification.            │
            ├────────────────────────────────────────────────────────────┤
  platform/ │ pico/   — THE ONLY code that includes pico-sdk/hardware/*.  │
            │ freertos/— the OSAL implementation.                         │
            │ host/   — mocks for unit tests (later).                     │
                            ▼
  core/     │ pure portable utilities, ZERO deps: Result, SpscRing,       │
            │ static_vector, function_ref, inplace_function, span/byte    │
            │ helpers, fixed string. Header-only, constexpr-friendly.     │
```

`vendor` (TinyUSB, Pico-PIO-USB, FatFs, FreeRTOS kernel) sits beside `platform/` — these
are C/C++ libraries we pin and wrap, not rewrite (see
[`../../docs/usb-stack-saga.md`](../../../docs/usb-stack-saga.md)). "Minimal Pico-SDK
dependency" is a rule for *our* code; the vendored USB stack stays.

## Key type decisions

These reconcile "modern C++" with "no heap on a dual-M0+ with a hard-real-time core."

- **Callables → non-allocating.** `std::function` heap-allocates and can hide a lock —
  banned. Use `core::function_ref<R(Args...)>` (non-owning view, for callbacks not stored
  past the call) and `core::inplace_function<R(Args...), N>` (owning, fixed `N`-byte inline
  storage, `static_assert` on overflow) for stored callbacks. This is the embedded-correct
  realization of "use std::function-style callables."
- **Buffers/spans.** `std::span<T>` for every `(ptr,len)` boundary; `std::array<T,N>` for
  fixed storage; `core::static_vector<T,N>` for fixed-capacity / runtime-count. The USB
  wire structs stay `__attribute__((packed))` standard-layout, exposed as
  `std::span<std::byte>` + typed accessors — no raw indexing in logic.
- **Errors.** `core::Result<T,E>` for fallible ops (already built); `std::optional<T>`
  for "maybe"; `bool` only for genuinely binary, context-free results.
- **Wire reinterpretation.** `std::bit_cast` / `std::byte` / typed accessor methods,
  never C casts, for packet ↔ bytes. Keep `static_assert`s on wire sizes/offsets.
- **HAL binding.** `concept`s (`hal::GpioOutput`, `hal::Uart`, `hal::Clock`, …) describe
  the contract; `platform/pico/` provides concrete types; `app` selects them via a single
  `namespace hal_impl = platform::pico;` (or a `platform_config.h`). Logic takes them by
  concrete-aliased type or a constrained template — zero indirection, still swappable.
- **Cross-core state.** Keep the proven lock-free discipline (aligned word, single-store
  publish) but typed: `std::atomic<uint32_t>` with explicit memory orders *iff* it emits
  the same plain load/store on M0+ (verify), else documented `volatile`. No mutex on a
  core1 path, ever.

## Naming / layout conventions

- Directories mirror layers: `inc/core/`, `inc/osal/`, `inc/hal/`, `inc/service/`,
  `inc/driver/`, `src/platform/pico/`, `src/service/`, `src/driver/`, `src/app/`.
- Namespaces mirror layers: `orb::core`, `orb::osal`, `orb::hal`, `orb::service`,
  `orb::driver`, `orb::app`, `orb::platform::pico`. (The existing top-level `orb::`
  `Result`/`SpscRing`/wrappers migrate into `orb::core`/`orb::osal`.)
- One type per header where it's a real abstraction; `.cpp` only when out-of-line code is
  needed (most `core`/`hal` is header-only/`constexpr`).
- Every `extern "C"` shim goes through `inc/orb_c_api.h` and is named `*_cb` / matches the
  C symbol it implements.

## Migration plan (incremental, bottom-up; each step builds + is hardware-validatable)

Same discipline that carried the FreeRTOS port: leaf/portable pieces first, the
core1/USB-timing pieces last, on a foundation already proven. The CMake source list flips
per file as each `.c` becomes its layer's `.cpp`.

- **P0 — `core/`** (portable, zero-dep): land `function_ref`, `inplace_function`,
  `static_vector`, span/byte helpers; move `Result`/`SpscRing` under `orb::core`. Lowest
  risk; host-unit-testable.
- **P1 — `osal/`**: define the RTOS interface; the existing `StaticTask`/`StaticQueue`/
  `SoftwareTimer`/`Mutex` become `platform/freertos/` impls behind it.
- **P2 — `hal/` + `platform/pico/`**: concepts + value types; the pico implementation; the
  portability boundary established. Migrate the simple hardware users first — LED
  (`GpioOut`), the hub-RESET pulse, the dlog UART, the `timerawl` `Clock` — off raw SDK
  calls.
- **P3 — `service/`** (portable logic): xbox protocol, `MidiParser`, `DrumEngine`,
  `Guitar`, `InstrumentManager`, `AdapterState`, hi-hat. Behavior-preserving, reviewable,
  partly host-testable. This is the bulk of the "it's still C" surface.
- **P4 — `driver/`**: `XboxDevice`, `XboxHostController` (core1), `MscLogSink`,
  `SerialMidi`, `PioUsbHost` behind the `extern "C"` `tu*_cb` seam. HIGH risk (core1/USB
  timing) — hardware-validate each (physical reset + a song + controller-drop recovery).
- **P5 — `app/`**: `main`, the task bodies, a `UsbHost`/`Adapter` orchestration object,
  the boot/recovery sequence (keep the `busy_wait_ms` pre-scheduler invariant). Validate.
- **P6 — enforce + clean**: the boundary grep in CI; remove the last C TUs; ship config
  (drop dev-only hooks); update all docs.

Each phase is one or a few reviewed commits and must build clean on both board targets;
core1-touching phases (P4/P5) are gated on real-hardware validation, not SWD reset (see
the hardware caveats in [`../../CLAUDE.md`](../../../CLAUDE.md) and the port handoff
[`../FREERTOS-PORT.md`](../FREERTOS-PORT.md)).

## Portability boundary

A file under `core/`, `service/`, or a layer *interface* must not include any of:
`pico/*`, `hardware/*`, `pico_sdk`, `FreeRTOS.h`, `task.h`/`queue.h`/`timers.h`/`semphr.h`,
`tusb.h`/`class/*`/`device/*`/`host/*`, `ff.h`/`diskio.h`. Only `src/platform/**` and the
vendored seam may. P6 adds a build-time/CI check; until then it's a review gate.

## What does NOT change

- The USB behavior, descriptors, and protocol on the wire (this is a refactor).
- The FreeRTOS SMP topology: core1 runs only the USB host task; everything else core0
  ([`../FREERTOS-PORT.md`](../FREERTOS-PORT.md)). The orchestration moves into objects;
  the affinities/priorities/comms semantics are preserved.
- The pinned vendored library versions and the 120 MHz clock.
- The deferred, lock-free, per-core logging discipline.

## References

- [`../FREERTOS-PORT.md`](../FREERTOS-PORT.md) — task model, core0/core1 split, the timing
  gotchas every layer must respect.
- [`../features/cpp-overhaul.md`](../features/cpp-overhaul.md) — the first foundation steps
  (Result/SpscRing/wrappers/builders), subsumed by this doc.
- [`../../docs/usb-stack-saga.md`](../../../docs/usb-stack-saga.md) — why the vendored
  USB stack is pinned and wrapped, not replaced.
