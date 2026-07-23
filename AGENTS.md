# AGENTS.md — openrb-pico contributor & agent guide

Conventions any human or agent working in this repo must follow. This is the short,
enforceable list; the deep rationale lives in
[`docs/architecture.md`](docs/architecture.md) (authoritative architecture + the
FreeRTOS-SMP runtime model) and [`BUILDING.md`](BUILDING.md). Porting/design history is on
the [wiki](https://github.com/delabrcd/openrb-pico/wiki/Design-History).

## Target architecture — static dependency injection

The app is migrating off its C-esque style (global singletons + free-function forwarders +
raw pointers passed everywhere) to **proper dependency injection**. New and refactored code
MUST follow this; it is the direction, not yet uniformly realized.

- **100% statically allocated. No heap** — no `new`/`delete`/`malloc`, no dynamic
  containers. Every object has static storage duration; queues/tasks/timers use the
  `orb::osal` static wrappers.
- **One class per module, with an entry file, in its own namespace.** Each module's entry
  file owns that module's static instance(s) and exposes a namespaced `init()` and (where it
  runs a task) `task()`. Nothing else is public.
- **`main` is thin wiring only.** It pulls in the namespaced `init()` functions and task
  functions and starts the scheduler. No feature logic in `main`.
- **Constructor injection by reference.** Dependencies are constructed as static objects and
  injected by reference (or `std::reference_wrapper`) at construction, wired in a single
  composition root. Do NOT reach for a hidden global via a free-function forwarder.
- **No raw pointers passed anywhere in our code.** Use references, `std::span`,
  `std::optional`, `std::reference_wrapper`. Owning/borrowing intent is expressed in the
  type, never a bare `T*`.
- **C-ABI seams are abstracted.** TinyUSB `extern "C"` callbacks and FreeRTOS `void*` task
  entries are the ONLY places a raw pointer may appear, and they are confined to one adapter
  TU per seam that converts to typed events / `std::span` and dispatches inward. Feature code
  never sees the raw pointer.
- **Time is `std::chrono` at every boundary.** Durations and timeouts are typed
  `std::chrono::duration`; timestamps are `Clock::time_point`. `TickType_t`, `pdMS_TO_TICKS`,
  `portMAX_DELAY` and bare `uint32_t`-milliseconds constants never appear outside `osal/`
  (the RTOS-tick conversion in `osal/chrono.hpp::to_ticks`) — everything above osal speaks
  `std::chrono`. The one deliberate exception is wire/descriptor fields that carry a numeric
  time value on the USB ABI (e.g. `bInterval`/`PollingIntervalMS`, packet `triggered_time`),
  which stay their raw integer type because they ARE the wire encoding, not our clock API.
- **Logging is the ONE sanctioned ambient module** (the assert-like exception to "everything
  is a System-owned, injected class"). `modules/log/` is deliberately NOT a System member and
  NOT injected: the `LOG_*` macros must be callable from every layer (core → app), from both
  cores, on the PIO-USB hot path, and before/without any object graph — so the emit path reads
  `constinit` module-static state (level table + the deferred SPSC ring) directly, lock-free,
  no indirection. Making it System-owned would force the `log` layer to reach up into `app`
  (a layering violation) and add a bound-pointer hop to every log call. Runtime config
  (`orb::log::set_level`/`set_cat_level`) mutates that same relaxed-atomic state. The vendor
  seams it does own (TinyUSB `tuh_msc_*` + FatFs `disk_*` for the USB-stick sink) still get the
  typed-anchor seam treatment like any other C-ABI seam.

## Language & style

- **C++23** (`CMAKE_CXX_STANDARD 23`). CppCoreGuidelines, with the pragmatic exceptions
  embedded requires (no exceptions/RTTI, no heap).
- **`enum class` exclusively** — never `typedef enum`. Convert at the boundary with
  `std::to_underlying`; type wire fields with the scoped enum rather than scattering
  `static_cast`.
- Prefer `constexpr`/CMake build profiles over hard-coded `#define`s. Macros only where a
  `#if`/compile-out genuinely requires them (log level floor, feature gates).

## Layering (enforced)

Modules live under `modules/<layer>/` (core, osal, hal, platform, log, protocol, service,
driver, board, app). Link edges only point downward. **Only `platform/pico/**` and the
vendor seam may include pico-sdk / TinyUSB**; `service/` and `protocol/` must not. This is
enforced by the INTERFACE-source link graph and `scripts/check-boundary.sh`.

## Concurrency (RP2040 dual Cortex-M0+, FreeRTOS SMP)

- **core1 runs ONLY `usb_host_task`** (PIO-USB bit timing). Never block it: no `sleep_ms` /
  `board_millis` / SDK spinlocks / unbounded mutex takes on core1. Use `timer_hw->timerawl`
  as the lock-free clock.
- **Feature logic runs on core0.** USB mount/umount callbacks fire on core1 — they must only
  post a non-blocking event to a core0 task, never do device work or take a cross-core lock
  inline. (See the instrument event queue in `service/instrument_manager.cpp` for the
  pattern.)
- `std::atomic` on M0+: relaxed load/store only (no LDREX/STREX → an RMW emits an
  IRQ-masking libcall). A cross-core check-then-set needs a critical section — but prefer a
  single-writer design that removes the need for one.

## Verification gate (every change must pass)

- `scripts/build.sh` builds **both** targets (`openrb-pico_FEATHER` and
  `openrb-pico_CUSTOM_REV_0_1`) in Docker — both must be green. (`build.sh clean` can't wipe
  the root-owned Docker build dir; use the incremental `build.sh` — ninja rebuilds changed
  TUs reliably.)
- For pure refactors, confirm **byte-identical** images via `arm-none-eabi-size` before/after.
  For functional changes, confirm sizes are sane and no region overflows.
- Host tests: configure `test/` into a fresh dir and run `ctest` — must pass.
- `scripts/check-boundary.sh` must pass. `scripts/check.sh` runs in-container clang-tidy.

## Hardware-testing caveats (honor these)

- **SWD `reset run` ≠ the physical RESET button** for USB-wedge behavior; ground truth is a
  physical reset + the controller LED.
- **The CH334R hub stays powered across RP2040 resets** and accumulates wedge state — power-
  cycle it between A/B candidates; keep reset batches small.
- Debug UART is the dlog probe on GPIO24/25 (`scripts/uart.sh`); a USB stick sink writes
  `LOG.TXT`.
