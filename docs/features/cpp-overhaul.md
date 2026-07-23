# Feature: C++ object-oriented overhaul
Status: IN PROGRESS — Phases 0,1,2,3a,3b,6 + foundation hardening + `namespace orb`
unification done and reviewed (see git log). Remaining: D4 hardware RAII
(`GpioOut`/`HubReset`/`MscLogSink`), D5/Phase 5 `adapter_ctx`→`AdapterCtx`, Phase 7
device driver, Phase 8 host driver, Phase 9 task/`UsbHost` objects — all core1/driver,
to be done step-by-step WITH hardware validation.
Related: [logging](logging.md), [runtime-configurator](runtime-configurator.md), [hihat-mode](hihat-mode.md)

## Summary

Incrementally re-express the firmware as modern, embedded-appropriate C++17: wrap the
bare C APIs (FreeRTOS, TinyUSB, the SPSC rings, the xbox protocol) in RAII objects,
replace ad-hoc buffer plumbing with fixed-capacity container types, and turn the
"raw global volatile + manual init/teardown" idioms into objects that own their
resources. The build is *already* C++17 (`project(openrb-pico C CXX ASM)`,
`set(CMAKE_CXX_STANDARD 17)` in [`CMakeLists.txt`](../../CMakeLists.txt):20,23) and one
C++ piece is established (`StaticTask<N>` in
[`inc/static_task.hpp`](../../inc/static_task.hpp), used from
[`src/app_tasks.cpp`](../../src/app_tasks.cpp)), so this extends a path that's already
open rather than introducing C++ from scratch.

This is explicitly **not** "rewrite in idiomatic desktop C++." It is a no-heap,
no-exceptions, code-size-aware overhaul that must preserve every concurrency and timing
invariant the FreeRTOS SMP port established (see
[`../FREERTOS-PORT.md`](../FREERTOS-PORT.md)). The C TinyUSB/FreeRTOS callback boundary
stays C-linkage forever.

## Motivation

- **The superloop-era patterns survived the port.** Cross-core state is a wall of
  `static volatile` globals ([`src/adapter_ctx.c`](../../src/adapter_ctx.c),
  `g_host_last_rx_us` / `g_runtime_recovery_engaged` / `g_recov_count` in
  [`src/main.c`](../../src/main.c)), each carrying a paragraph of comment explaining the
  lock-free contract that the *next* edit can silently break. Wrapping the contract in a
  type makes the discipline enforceable instead of conventional.
- **Resource lifecycles are manual and duplicated.** `gpio_init`/`gpio_set_dir`,
  `dma_claim_unused_channel` + immediate `dma_channel_unclaim`
  ([`src/main.c`](../../src/main.c):309-310), `f_mount`/`f_open`/`f_close`/`f_mount(0,...)`
  ([`src/usb_log.c`](../../src/usb_log.c)), `uart_init`+`gpio_set_function` — all
  open-coded with the teardown easy to forget or mis-order. RAII fixes the
  acquire/release pairing structurally.
- **Buffer plumbing is copy-paste.** The SPSC ring discipline (power-of-two mask, two
  volatile cursors, drop-on-full, "aligned 32-bit load/store is atomic on M0+") is
  hand-reimplemented in [`src/dlog.c`](../../src/dlog.c) and
  [`src/usb_log.c`](../../src/usb_log.c); the device fifo is a *third* mechanism via the
  `CREATE_GENERIC_FIFO` macro ([`inc/generic_fifo.h`](../../inc/generic_fifo.h)). One
  tested ring class would replace three near-identical implementations.
- **The two TinyUSB drivers are parallel hand-rolled state machines.**
  `xbox_interface_t` / `_xbox_itf[]` ([`src/xbox_controller_driver.c`](../../src/xbox_controller_driver.c))
  and `xinputd_interface_t` / `_xinputd_itf[]`
  ([`src/xbox_device_driver.c`](../../src/xbox_device_driver.c)) repeat the same
  find-slot / claim-endpoint / xfer / release pattern. An endpoint wrapper removes the
  claim/release-leak class of bug.

## Goals / Non-goals

### Goals
- RAII wrappers over the FreeRTOS primitives actually in use: task (extend
  `StaticTask<N>`), static queue, mutex, software timer, task notification.
- A single reusable, lock-free SPSC ring type (replaces the dlog/usb_log rings and the
  `GENERIC_FIFO` macro for the device fifo).
- Fixed-capacity container types (`std::array`/`std::span` + a `static_vector`-style type)
  for the buffer/packet plumbing, with **zero dynamic allocation**.
- Objects for hardware resources: GPIO line, the hub-RESET# pulse, the USB-host config,
  the FatFs log sink, the two USB interface tables.
- A typed wrapper around the cross-core `adapter_ctx` state that *preserves* the existing
  lock-free packed-word discipline.
- A clean, permanent `extern "C"` shim layer at the TinyUSB/FreeRTOS boundary.
- Each migration step builds and is testable on hardware.

### Non-goals
- No `std::vector` / `std::string` / `std::function` / `std::shared_ptr` / `new`/`delete`
  / streams / RTTI / exceptions (rationale below).
- No change to behavior, USB protocol, task topology, priorities, or core affinity. This
  is a structural refactor, not a feature change.
- No clock, IRQ-priority, or timing-tuning changes (those are the separate Phase-5 items
  in [`../FREERTOS-PORT.md`](../FREERTOS-PORT.md)).
- Not rewriting the vendored TinyUSB / Pico-PIO-USB / FatFs / FreeRTOS sources — they
  stay C and stay at their pinned versions (see [`../../docs/usb-stack-saga.md`](../../../docs/usb-stack-saga.md)).
- No port of the sibling AVR `instruments` firmware.

## Current state (in code)

| Concern | Today | File |
|---|---|---|
| Task creation | `StaticTask<N>` template (trivial type, BSS stack+TCB) — the one existing C++ wrapper | [`inc/static_task.hpp`](../../inc/static_task.hpp), [`src/app_tasks.cpp`](../../src/app_tasks.cpp) |
| Cross-core state | `static volatile` state/packed-controller/alive/seen/reinit + free functions | [`src/adapter_ctx.c`](../../src/adapter_ctx.c) |
| Recovery state | `static volatile` `g_host_last_rx_us`, `g_host_rx_count`, `g_runtime_recovery_engaged`; function-local statics in recovery tasks | [`src/main.c`](../../src/main.c):57-64,355-544 |
| Inter-task queues | FreeRTOS static queues, raw byte storage arrays | [`src/app_queues.c`](../../src/app_queues.c) |
| Device TX fifo | `tu_fifo` + OSAL mutex wrapped by the `CREATE_GENERIC_FIFO`/`GENERIC_FIFO_EXPORTS` X-macro | [`inc/generic_fifo.h`](../../inc/generic_fifo.h), [`inc/packet_queue.h`](../../inc/packet_queue.h) |
| Log rings | two hand-rolled lock-free SPSC rings (per-core dlog; core0→core1 usb_log) | [`src/dlog.c`](../../src/dlog.c), [`src/usb_log.c`](../../src/usb_log.c) |
| MIDI disconnect timer | static FreeRTOS software timer (`xTimerCreateStatic`) | [`src/midi.c`](../../src/midi.c):49-61 |
| Host driver | `xbox_interface_t _xbox_itf[1]` + find-slot/claim/xfer/release; error-streak volatile | [`src/xbox_controller_driver.c`](../../src/xbox_controller_driver.c) |
| Device driver | `xinputd_interface_t _xinputd_itf[]` + `usbd_class_driver_t` vtable; `usbd_app_driver_get_cb` | [`src/xbox_device_driver.c`](../../src/xbox_device_driver.c) |
| Drum state | one `static struct drum_state` with input packet + per-output trigger state | [`src/drums.c`](../../src/drums.c):41-57 |
| Instrument table | `static volatile uint8_t connected_instruments[N]`; flash notify/dropout tables | [`src/instrument_manager.c`](../../src/instrument_manager.c) |
| Packet type | `xbox_packet_t` — packed union (`frame`/`wla_header`/typed inputs/`buffer[]`) + meta | [`inc/xbox_one_protocol.h`](../../inc/xbox_one_protocol.h):189-203 |

Build facts that constrain this work:
- C++17 is on; only `src/app_tasks.cpp` is currently a C++ TU.
- `FreeRTOS-Kernel-Heap4` is linked ([`cmake/AddBoardTarget.cmake`](../../cmake/AddBoardTarget.cmake):22)
  and `configSUPPORT_STATIC_ALLOCATION` is the convention — the heap exists but the
  project deliberately creates everything statically.
- **No `-fno-exceptions` / `-fno-rtti` is set today** (grep of `CMakeLists.txt` +
  `cmake/*.cmake` finds none). This must be added (see Design → Constraints).
- The project's own sources get an optional `-Og -g3` deep-backtrace variant; the
  vendored libs stay at default opt ([`cmake/AddBoardTarget.cmake`](../../cmake/AddBoardTarget.cmake):16-17).
- [`../../santroller`](../../../santroller) is a C++ RP2040 rhythm-controller project
  (class-per-input under `include/input/*.hpp`, `usb/{host,device}` namespaces) — use it
  as a *style* reference only; it has different lib versions/clock and is not ours.

## Design

### D1. FreeRTOS RAII wrappers

Extend the established `StaticTask<N>` pattern (own-the-storage, BSS-resident, trivially
constructible — its header comment already calls out "no global ctor / static-init-order
concerns"). Every wrapper keeps that property: **storage members are the FreeRTOS
`Static*_t` buffers, construction does nothing that touches the kernel, and a `create()`
/ `start()` method does the actual `xCreateStatic...` call** after the kernel is up
enough. This avoids static-init-order fiasco on a system with no heap and a hand-rolled
`main()`.

- `StaticTask<N>` — already exists; keep as-is, it's the template for the rest.
- `StaticQueue<T, Depth>` — wraps `xQueueCreateStatic`; replaces the raw
  `host_tx_storage` / `midi_note_storage` byte arrays and the four free functions in
  [`src/app_queues.c`](../../src/app_queues.c). API: `send(const T&)`, `recv(T&)` returning
  `bool`, both non-blocking (timeout 0) to match today's semantics.
- `Mutex` / `ScopedLock` — wraps `xSemaphoreCreateMutexStatic` + take/give with an RAII
  guard. NB: the device fifo's mutexes today live *inside* TinyUSB's OSAL via
  `tu_fifo_config_mutex` ([`inc/generic_fifo.h`](../../inc/generic_fifo.h):38); if the
  ring is reimplemented (D3) the OSAL mutex goes away — decide whether the new ring is
  lock-free SPSC (preferred, no mutex) or guarded.
- `SoftwareTimer` — wraps `xTimerCreateStatic` + `xTimerChangePeriod`; replaces the
  `s_disconnect_timer` / `s_disconnect_timer_buf` pair and `reset_disconnect_timer()`
  in [`src/midi.c`](../../src/midi.c). The timer callback stays a free `extern "C"`
  function (`on_disconnect_timeout_cb`, already `__not_in_flash_func`).
- `TaskNotification` — thin wrapper over `xTaskNotify*` for the cases currently polled.
  *Open question:* the comms are queue/poll based today; introducing notifications is a
  behavior change, so this wrapper is optional and should only land where it provably
  replaces a poll without altering timing.

The callback the kernel calls (timer cb, idle/tick hooks in
[`src/freertos_hooks.c`](../../src/freertos_hooks.c)) must remain `extern "C"`.

### D2. TinyUSB RAII wrappers (callbacks stay `extern "C"`)

TinyUSB calls *into* us through a fixed set of C-linkage symbols. Those **cannot** become
member functions or be name-mangled. The pattern is: keep a thin `extern "C"` shim that
immediately forwards into a C++ object that holds the state.

- **Class driver registration stays C.** `usbd_app_driver_get_cb` and the
  `usbd_class_driver_t _xboxd_driver` vtable
  ([`src/xbox_device_driver.c`](../../src/xbox_device_driver.c):259-274) are C structs of
  C function pointers — leave the registration C; the function pointers may point at
  `extern "C"` shims that forward into a `XboxDevice` object.
- **Host class driver hooks** (`xboxh_open`/`xboxh_xfer_cb`/`xboxh_close`/... in
  [`src/xbox_controller_driver.c`](../../src/xbox_controller_driver.c)) and the device
  hooks (`xboxd_open`/`xboxd_xfer_cb`/`xboxd_control_xfer_cb`) stay `extern "C"`.
- **Wrap the stateful bits, not the callbacks:**
  - `Endpoint` — owns an ep address + its `CFG_TU*_MEM_ALIGN xbox_packet_t` buffer and
    encapsulates the claim → `usbh_edpt_xfer`/`usbd_edpt_xfer` → release dance
    (today open-coded with manual `usbh_edpt_release` on every error path —
    [`src/xbox_controller_driver.c`](../../src/xbox_controller_driver.c):162-174). The
    claim is the RAII-guardable resource (a `ClaimedEndpoint` guard that releases on
    scope exit removes the leak-on-error-path bug class). The aligned buffers must keep
    their `CFG_TUH_MEM_SECTION` / `CFG_TUSB_MEM_SECTION` placement.
  - `XboxController` (host interface) — replaces `xbox_interface_t` + the
    `find_new_itf`/`get_xbox_itf`/`get_idx_by_epaddr` free functions; holds daddr,
    itf_num, the two `Endpoint`s, VID/PID, `is_powered`, and the IN-error-streak counter
    (today `s_in_err_streak`, a `volatile` that *must* stay core1-only / lock-free —
    see the comment at [`src/xbox_controller_driver.c`](../../src/xbox_controller_driver.c):408-416).
  - `XboxDevice` (device interface) — replaces `xinputd_interface_t` + `_xinputd_itf[]`
    and centralizes the `epin_buf.handled` / TX-drain handshake in `xboxd_send_task`.
  - `UsbHost` — owns `configure_host()`'s setup (PIO-USB config, the
    `dma_claim_unused_channel`/`dma_channel_unclaim` probe, `tuh_configure`/`tuh_init`).
  - `MscLogSink` — owns the FatFs `FATFS`/`FIL` lifecycle in
    [`src/usb_log.c`](../../src/usb_log.c) (mount/open in ctor-like `attach()`,
    close/unmount in `detach()`), replacing the `s_fatfs`/`s_file`/`s_fs_ready`/`s_dev_addr`
    statics. The `tuh_msc_mount_cb`/`umount_cb` and the `disk_*` diskio glue stay
    `extern "C"`.

The xbox **protocol/packets**: keep `xbox_packet_t` as the wire-layout
`__attribute__((packed))` union it is today ([`inc/xbox_one_protocol.h`](../../inc/xbox_one_protocol.h):189-203)
— the packing and the union are load-bearing for the USB layout, so it must stay a
standard-layout aggregate (do not add virtuals/ctors that would change layout or make it
non-trivial). Add C++ *builder/accessor* helpers around it (e.g. typed packet factories
to replace the `power_report_t out = {.data = {.frame = {...}}}` designated-initializer
blocks in [`src/xbox_controller_driver.c`](../../src/xbox_controller_driver.c)) rather
than changing the type.

### D3. Containers / buffers — embedded-correct

**Hard rule: no dynamic allocation.** Static allocation is the project convention
(`configSUPPORT_STATIC_ALLOCATION`, `xQueueCreateStatic`, `xTimerCreateStatic`,
`StaticTask<N>`'s own-the-buffer design, `tu_static`/`CFG_TU*_MEM_SECTION` buffers). The
reasons are concrete here, not dogma:
- **core1 timing.** core1 runs the bit-banged PIO-USB and must never block; a heap that
  takes a lock (or a malloc that touches a structure another core is in) is exactly the
  "hidden lock on core1" the port forbids ([`../FREERTOS-PORT.md`](../FREERTOS-PORT.md)
  gotchas #1, #2). `new`/`std::vector` growth would smuggle that in.
- **Determinism / no fragmentation** on a long-running device that must survive a full
  song without a heap event.
- **Footprint.** Pulling in the C++ allocator path / `std::vector` growth also tends to
  drag in exception machinery.

Container choices:
- `std::array<T, N>` — for the genuinely fixed buffers (the flash notify/dropout tables
  in [`src/instrument_manager.c`](../../src/instrument_manager.c), `out_packet`, the
  per-output trigger state array `midi_output_states[NUM_OUT]` in
  [`src/drums.c`](../../src/drums.c)).
- `std::span<T>` — for passing buffer+length pairs across the API instead of
  `(uint8_t*, len)` (e.g. `usb_log_write`, `dlog_sink`, the report send paths). Zero
  cost, no ownership, keeps the C ABI shims simple.
- `SpscRing<T, N>` (new, header-only, `N` power-of-two) — the single lock-free
  single-producer/single-consumer ring that replaces the two hand-rolled rings in
  [`src/dlog.c`](../../src/dlog.c) / [`src/usb_log.c`](../../src/usb_log.c). It must
  reproduce *exactly* today's contract: aligned `volatile uint32_t` head/tail, producer
  and consumer touch different words, drop-the-rest on full (never block), and the buffer
  in BSS. This is the highest-value single wrapper. Consider `std::atomic<uint32_t>` with
  explicit `memory_order_relaxed`/`release`/`acquire` instead of bare `volatile` — it
  documents the contract and is correct on M0+ — but verify it generates the same plain
  load/store (no library call) before adopting.
- `static_vector<T, N>` (etl-style or hand-rolled) — for the few "fixed capacity, runtime
  count" cases (e.g. the interface tables, currently `[1]`/`[CFG_TUD_XINPUT]`). Low
  priority while those arrays are size-1.
- **Avoid** `std::vector`, `std::string`, `std::deque`, `std::function`. For the device
  TX path, replace the `tu_fifo`+OSAL-mutex `GENERIC_FIFO` macro with either `SpscRing`
  (if access is genuinely one-producer/one-consumer) or a `StaticQueue<xbox_packet_t,N>`
  — **note** the device fifo today is documented as *multi-writer* (both cores →
  `usb_device_task`, [`../FREERTOS-PORT.md`](../FREERTOS-PORT.md) comms table), so it
  needs the write mutex; SPSC is *not* a drop-in there. This is a real decision (open Q).

Whether to vendor a small subset of ETL vs. hand-roll `SpscRing`/`static_vector`: prefer
hand-rolling the two types we need (keeps the dependency surface and code size minimal),
matching how `StaticTask<N>` was hand-rolled.

### D4. RAII / ownership of hardware resources

- `GpioOut` / `GpioLine` — wraps `gpio_init`+`gpio_set_dir`+`gpio_put` (the LED at
  [`src/main.c`](../../src/main.c):86,596-597, `PIN_5V_EN` in `set_usb_host`).
- `HubReset` — the active-low RESET# pulse in `reset_usb_hub()`
  ([`src/main.c`](../../src/main.c):471-484) as an object/method. **Critical invariant to
  preserve:** it uses `busy_wait_ms` (NOT `sleep_ms`) because it runs pre-scheduler and
  is reused on core1; and it releases the pin to **Hi-Z** (never drives high) to avoid
  the CH334R CDP mode. A wrapper must keep both. Do not let RAII "tidiness" turn the
  pulse into a driven-high.
- `DmaChannel` — even though `configure_host()` only *probes* for a free channel and
  immediately unclaims it ([`src/main.c`](../../src/main.c):309-310), a scoped
  claim/unclaim guard documents intent. (Low priority; the probe is a quirk of the
  PIO-USB config API.)
- The two **interface tables** become owned members of `UsbHost`/`XboxDevice` rather than
  file-scope `tu_static` arrays.

**Cross-core volatiles are the explicit exception.** `adapter_ctx`
([`src/adapter_ctx.c`](../../src/adapter_ctx.c)) and the recovery
timestamps/flags in [`src/main.c`](../../src/main.c) are touched by both cores with a
*deliberate* lock-free discipline (packed 32-bit word so a replug is one atomic store and
core0 never sees a torn idx/addr pair; single-volatile bools that are atomic on M0+). A
C++ wrapper here must **preserve, not hide, that contract**: model it as e.g.
`class AdapterCtx` with `std::atomic<uint32_t> controller_` (the packed word),
`std::atomic<bool>` flags, and methods named for the operations
(`get_controller(idx,addr)`, `set_controller`, `take_reinit`). Do **not** add a mutex
(it would put a lock on core1) and do **not** widen the packed word in a way that breaks
the single-store atomicity. The `take_reinit` read-clear is a benign race today; if it
becomes an atomic, use `exchange(false)` to make the intent explicit.

### D5. Embedded C++ constraints (call these out in the build)

- **`-fno-exceptions -fno-rtti`** — add to the project's own sources
  ([`cmake/AddBoardTarget.cmake`](../../cmake/AddBoardTarget.cmake), where the per-target
  options already live). Implications: no `throw`/`try`, no `dynamic_cast`/`typeid`; all
  the wrappers report failure by return value/`bool`/`optional`-like, exactly as the C
  code does today (`TU_VERIFY`, `bool` returns). This also keeps the unwind tables we add
  for `-Og` debugging from being dragged in by EH. (We already build with
  `-funwind-tables -fasynchronous-unwind-tables` for backtraces — those are for the
  debugger, independent of C++ EH.)
- **No heap from C++.** With `new`/`delete` unused, optionally provide trapping
  `operator new`/`delete` (call `panic`/breakpoint) so an accidental allocation is caught
  at link/run rather than silently pulling Heap4. `-fno-threadsafe-statics` is also worth
  setting: function-local statics otherwise emit `__cxa_guard_*` calls (a hidden lock) —
  unacceptable on core1. Audit any new function-local `static` accordingly.
- **Code size awareness.** Templates instantiate per type/size — prefer non-template base
  classes with thin templated façades where it matters (e.g. a `SpscRingBase` operating
  on `void*`+elem-size, with `SpscRing<T,N>` a typed shell) to avoid N copies of the
  drain loop. Measure `.text`/`.bss` before/after each phase (the build already produces a
  map; `arm-none-eabi-size` per phase).
- **Section/placement attributes survive.** `__not_in_flash_func` (RAM execution for
  core1 hot paths — `drum_task`, `drums_read_midi_host`, `tusb_time_delay_ms_api`, the
  timer cb), `__in_flash()`, `CFG_TU*_MEM_SECTION`/`TU_ATTR_ALIGNED`, and `tu_static`
  must be preserved on the wrapped members. C++ member functions can carry
  `__not_in_flash_func`; verify the mangled symbol lands in the right section. No-init/BSS
  placement of the static wrapper instances must be confirmed (the `StaticTask<N>` header
  already documents "stacks + TCBs land in BSS").
- **C callbacks stay C-linkage.** Every TinyUSB `tu*_cb` / `usbd_app_driver_get_cb` /
  diskio `disk_*` / FreeRTOS hook / `tusb_time_delay_ms_api` override remains
  `extern "C"`. A single `extern "C"` shim header is the seam.
- **ISR / core1 timing rules (from [`../FREERTOS-PORT.md`](../FREERTOS-PORT.md)) are
  non-negotiable:** core1 code must not call `sleep_ms`/`board_millis`/SDK spinlocks, must
  use `timer_hw->timerawl` and `busy_wait_ms`, and must not take a lock or allocate. Any
  C++ added to the core1 path (the host task, `XboxController`, `SpscRing` producer/consumer
  split, `MscLogSink`) inherits all of this. No constructor that runs on core1 may touch
  the kernel or a spinlock.

### D6. Style

Follow the established local convention (`StaticTask<N>`: own storage, trivial ctor,
explicit `start()`), and use [`../../santroller`](../../../santroller) (namespaced
`usb::host` / `usb::device`, class-per-concern) as a *readability* reference — not as an
architecture to copy wholesale, since its USB/clock setup differs from ours.

## Migration plan / phasing

Each phase is one or a few commits, must **build clean and run a full song + a
controller-drop recovery on real hardware** before the next (SWD `reset run` is not
representative — physical power-cycle test per the hardware caveats in
[`../../docs/usb-stack-saga.md`](../../../docs/usb-stack-saga.md) and openrb
[`CLAUDE.md`](../../../CLAUDE.md)). Order is leaf utilities → modules → tasks so the risky
core1/USB pieces come last on a foundation that's already proven.

0. **Build prep (no behavior change).** Add `-fno-exceptions -fno-rtti
   -fno-threadsafe-statics` to the project sources; optionally the trapping
   `operator new`. Confirm size delta ≈ 0 and it still boots. Establish the
   `arm-none-eabi-size` baseline.
1. **`SpscRing<T,N>` + `extern "C"` shim seam.** Introduce the ring header and the
   boundary shim header. No callers changed yet.
2. **Leaf utility migration:** convert one log ring (`dlog`, core-local, lowest risk)
   to `SpscRing`. Validate. Then `usb_log`'s ring (core0→core1) — validate the lock-free
   contract on hardware (logs still land on the stick, no core1 stall).
3. **FreeRTOS wrappers:** `StaticQueue<T,Depth>` → rewrite
   [`src/app_queues.c`](../../src/app_queues.c) behind the same `host_tx_*`/`midi_note_*`
   free-function API (or replace call sites). Then `SoftwareTimer` → `src/midi.c`. Then
   `Mutex`/`ScopedLock` where needed.
4. **Hardware RAII:** `GpioOut`, `HubReset`, `MscLogSink` (wrap `usb_log.c`'s FatFs
   state). Keep the `busy_wait_ms`/Hi-Z invariants. Validate recovery still pulses the hub.
5. **`adapter_ctx` → `AdapterCtx` object** with atomics. Highest-care concurrency step;
   validate replug/recovery heavily. Fold the `g_host_*`/`g_runtime_recovery_engaged`
   recovery state in [`src/main.c`](../../src/main.c) into objects too.
6. **xbox protocol helpers:** packet builders/accessors around `xbox_packet_t` (no layout
   change); replace the designated-initializer blocks.
7. **Device driver → `XboxDevice` + `Endpoint`** behind the existing `extern "C"`
   `xboxd_*` hooks. Validate enumeration to the console.
8. **Host driver → `XboxController` + `Endpoint`/`ClaimedEndpoint`** behind the
   `extern "C"` `xboxh_*` hooks. This touches core1 — do last, validate the IN-error-streak
   recovery and timing most carefully.
9. **Task bodies / `UsbHost` object** in [`src/main.c`](../../src/main.c) — the loops can
   become methods, but the task entry points stay `extern "C"` `TaskFunction_t`. The
   device-TX fifo decision (D3) lands here.

Each `.c` becomes `.cpp` only when its turn comes; the file stays compilable C until then.
The CMake source list ([`CMakeLists.txt`](../../CMakeLists.txt):78+) flips per file.

## Risks & open questions

**Risks**
- **Code size / template bloat.** Per-`<T,N>` instantiation of `SpscRing`/`StaticQueue`
  could grow `.text`. Mitigate with a type-erased base + measure each phase. Risk that
  the win in readability costs flash we don't have spare.
- **The C/C++ boundary.** Easy to accidentally mangle a callback or change a struct's
  layout (`xbox_packet_t`, the `usbd_class_driver_t` vtable). A regression here = a device
  that won't enumerate. Mitigation: the boundary is a single `extern "C"` shim header;
  `static_assert` the packet sizes (the disabled assert at
  [`inc/xbox_one_protocol.h`](../../inc/xbox_one_protocol.h):205 should be re-enabled and
  fixed as part of phase 6).
- **Timing-sensitive regressions on core1.** A hidden lock (`__cxa_guard`, an OSAL mutex
  smuggled in by a "tidy" ring), a constructor doing real work, or a non-`__not_in_flash`
  hot function could re-introduce the exact wedge classes the port fought
  ([`../FREERTOS-PORT.md`](../FREERTOS-PORT.md) gotchas). Mitigation: phase the core1
  pieces last, keep `-fno-threadsafe-statics`, hardware-validate recovery each step.
- **Concurrency regression in `adapter_ctx`.** Replacing volatiles with atomics is correct
  *if* it stays single-store on the packed word and lock-free; getting the memory orders
  or the packing wrong reintroduces torn reads on replug. Mitigation: keep the exact
  packed-word scheme, heavy replug testing.
- **Scope creep.** "OO overhaul of *every* task/function" can balloon. The phasing caps
  each step; resist rewriting the protocol or task model.

**Open questions (for the owner)**
1. **Device TX fifo** is multi-writer (both cores → `usb_device_task`). Keep the
   `tu_fifo`+OSAL mutex (wrap it), move to a guarded `StaticQueue`, or restructure so
   only one core enqueues? SPSC is *not* valid here. This is the one genuine
   multi-producer structure.
2. **ETL vs. hand-rolled** for `SpscRing`/`static_vector` — vendor a tested library, or
   hand-roll the 2 types we actually need (matches the `StaticTask` precedent)?
3. **`std::atomic` vs. `volatile`** for the lock-free cursors and `adapter_ctx` — adopt
   `std::atomic<uint32_t>` (documents intent, correct on M0+) only if it provably emits
   the same plain load/store as today's `volatile`. Verify on the actual toolchain first.
4. **Trapping `operator new`/`delete`** — install them to hard-fail on accidental
   allocation? (Recommended, but it's a policy call.)
5. **How far to push `TaskNotification`** — today's design is queue/poll. Introducing
   notifications changes runtime behavior; do we want that in a "structural only" refactor,
   or defer to a separate feature?
6. **Re-enable the `xbox_packet_t` size `static_assert`** (currently commented out) — is
   the intended invariant `sizeof == XBOX_ONE_EP_MAXPKTSIZE`, or is the trailing
   meta (`length`/`triggered_time`/`handled`) deliberately outside the wire size? Needs a
   decision before phase 6.

## References

- [`../FREERTOS-PORT.md`](../FREERTOS-PORT.md) — task model, core0/core1 split, comms
  table, the core1 timing gotchas this overhaul must preserve.
- [`../../docs/usb-stack-saga.md`](../../../docs/usb-stack-saga.md), openrb
  [`CLAUDE.md`](../../../CLAUDE.md) — pinned lib versions and the hardware-testing caveats
  (physical reset/power-cycle, not SWD).
- Code (current state):
  [`inc/static_task.hpp`](../../inc/static_task.hpp),
  [`src/app_tasks.cpp`](../../src/app_tasks.cpp),
  [`src/app_queues.c`](../../src/app_queues.c),
  [`src/adapter_ctx.c`](../../src/adapter_ctx.c),
  [`src/main.c`](../../src/main.c),
  [`src/dlog.c`](../../src/dlog.c),
  [`src/usb_log.c`](../../src/usb_log.c),
  [`src/drums.c`](../../src/drums.c),
  [`src/midi.c`](../../src/midi.c),
  [`src/instrument_manager.c`](../../src/instrument_manager.c),
  [`src/xbox_controller_driver.c`](../../src/xbox_controller_driver.c),
  [`src/xbox_device_driver.c`](../../src/xbox_device_driver.c),
  [`inc/generic_fifo.h`](../../inc/generic_fifo.h),
  [`inc/packet_queue.h`](../../inc/packet_queue.h),
  [`inc/xbox_one_protocol.h`](../../inc/xbox_one_protocol.h),
  [`CMakeLists.txt`](../../CMakeLists.txt),
  [`cmake/AddBoardTarget.cmake`](../../cmake/AddBoardTarget.cmake).
- [`../../santroller`](../../../santroller) — third-party C++ RP2040 controller, style
  reference only.
</content>
</invoke>
