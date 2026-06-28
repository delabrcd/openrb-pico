# FreeRTOS SMP port — handoff

Status: **functionally complete and validated on hardware** (enumerates, streams input,
survives a full song of real gameplay, recovers a dropped controller without a reboot).
Work lives on branch `feature/freertos-smp-port` (pushed to `origin`). What remains is
deliberate tuning/hardening (see [What's left](#whats-left)), not core functionality.

This doc is the entry point for the port. For build/flash/debug mechanics see
[`../BUILDING.md`](../BUILDING.md) and [`DEBUGGING.md`](DEBUGGING.md); for the older
PIO-USB-on-pico-sdk saga see [`../PORTING.md`](../PORTING.md) and
[`../../docs/usb-stack-saga.md`](../../docs/usb-stack-saga.md).

## Why

The firmware had grown into a dual-core bare-metal superloop (core0 polled the USB
device stack + all feature work; core1 ran a dedicated `tuh_task()` loop for the
PIO-USB host). As features piled up, the superloop got hard to reason about. The port
moves to a **FreeRTOS SMP task model** so each concern is an independent, prioritized
task — without disturbing the hard-won PIO-USB timing on core1.

## Topology (the one thing to internalize)

- **FreeRTOS SMP**, both cores under the scheduler (`configNUMBER_OF_CORES=2`,
  `configUSE_CORE_AFFINITY=1`). The kernel is the official FreeRTOS-Kernel V11.2.0
  RP2040 SMP port (`external/FreeRTOS-Kernel`, `portable/ThirdParty/GCC/RP2040`).
- **core1 runs exactly ONE task — `usb_host_task`** — so the bit-banged PIO-USB
  signalling sees ~no context switches. The FreeRTOS timer-service daemon is pinned to
  core0 for the same reason (`configTIMER_SERVICE_TASK_CORE_AFFINITY`).
- **Everything else is pinned to core0.** `vTaskStartScheduler()` launches core1 itself
  (no more `multicore_launch_core1`).
- **TinyUSB uses `OPT_OS_FREERTOS`** (`inc/custom_config.h`), so `tud_task`/`tuh_task`
  block on their OSAL event queues instead of busy-polling.

### Tasks

| Task | Core | Prio | Blocks on | Role |
|---|---|---|---|---|
| `usb_host_task` | 1 (only task there) | 6 | `tuh_task_ext(10ms)` | PIO-USB host: enumerate controller via CH334R hub; host-TX drain; USB-MIDI read; usb_log; controller reinit + **runtime recovery** |
| `usb_device_task` | 0 | 6 | `tud_task_ext(4ms)` | USB device stack (Xbox-One drums to console) + `xboxd_send_task` (drain `device_tx`→IN endpoint). Kept together: both touch the device endpoint, which must be serialized |
| `drum_input_task` | 0 | 5 | `vTaskDelay(2ms)` | Instrument input: drain `midi_note` queue (USB-MIDI, filled on core1) + serial MIDI, age hits, write drum packets |
| `housekeeping_task` | 0 | 4 | `vTaskDelay(5ms)` | Periodic chores: announce heartbeat, warm-reset recovery, deferred-log drain |

Task creation is centralized in [`src/app_tasks.cpp`](../src/app_tasks.cpp) via the
`StaticTask<N>` template ([`inc/static_task.hpp`](../inc/static_task.hpp)), which owns
each task's stack + TCB (static allocation, no heap). Bodies are in
[`src/main.c`](../src/main.c).

### Inter-core / inter-task comms

| Channel | Direction | Backing | Notes |
|---|---|---|---|
| `device_tx` (`xbox_fifo`) | both cores → `usb_device_task` | `tu_fifo` + OSAL (FreeRTOS) write mutex, single reader | console-bound packets (controller input, identify, announce, instrument notify) |
| `host_tx_queue` | core0 device handlers → `usb_host_task` (core1) | FreeRTOS queue | moves all `xboxh_send` onto core1 — host stack is only ever touched on core1 |
| `midi_note` queue | `usb_host_task` (core1) → `drum_input_task` (core0) | FreeRTOS queue | moves `tuh_midi_stream_read` onto core1; core0 only consumes parsed notes |
| `adapter_ctx` | both cores | aligned volatiles (lock-free on M0+) | state machine + packed controller idx/addr + alive/seen + reinit signal. [`inc/adapter_ctx.h`](../inc/adapter_ctx.h) |
| `dlog` rings | each core → core0 drain | lock-free SPSC | deferred debug log; drained by `housekeeping_task` |
| `usb_log` ring | core0 → core1 | lock-free SPSC | log-to-USB-stick; core1 does the FatFs writes |

### Runtime controller recovery (non-reboot)

A wedged CH334R hub (e.g. after a debug halt freezes SOF, or any transient glitch)
leaves the controller a **silent zombie** — still "mounted" but its interrupt-IN poll
fails continuously, and TinyUSB never sees an umount. `host_recovery_task` (in
`usb_host_task`, core1) detects this two ways and pulses the hub's RESET# to force a
clean re-enumeration **without rebooting**:

- **Fast (primary):** a run of consecutive interrupt-IN failures
  (`xboxh_in_error_streak()` ≥ 100, ~1.25 s). A healthy *idle* pad produces no IN
  completions between its sparse ~20 s `CMD_STATUS` heartbeats, so the streak is
  false-positive-free. Recovers ~1–3 s after a wedge.
- **Backstop:** 30 s of total silence, in case a wedge ever stops the poll entirely
  rather than failing it (the ~20 s idle heartbeat refreshes the timer, so it doesn't
  false-trigger).

The old watchdog-reboot recovery (`recovery_reboot_task`) is now the **last resort** —
it defers to the runtime path via `g_runtime_recovery_engaged`. See
[`warm-reset-recovery.md`](warm-reset-recovery.md).

## Critical lessons / gotchas (read before touching core1 or boot)

1. **No pico-time `sleep_ms`/`sleep_us` before `vTaskStartScheduler()`.** With the
   FreeRTOS pico-time interop, `sleep_ms` blocks at the FreeRTOS level
   (`xEventGroupWaitBits`) which can never wake pre-scheduler → **intermittent boot
   deadlock** (core0 hangs in `init()`, core1 never launches, stuck in the bootrom).
   Pre-scheduler delays use `busy_wait_ms` (see `reset_usb_hub`). This was the single
   nastiest bug of the port.
2. **core1 must never use `sleep_ms`/`board_millis`/SDK spinlocks** — they hang the
   PIO-USB core. Use `timer_hw->timerawl` (lock-free) for timing and `busy_wait_ms` for
   delays. The `tusb_time_delay_ms_api` override exists for exactly this.
3. **Debug halts drop the controller.** Halting both cores freezes SOF → the hub wedges.
   The runtime recovery brings it back ~1–3 s after resume — expect a
   `HOST RECOVERY: controller wedged -> hub reset` + reconnect in the UART, no reboot.
4. **The debug daemon must not auto-probe flash.** `docker/openocd-daemon.sh` sets
   `gdb_memory_map disable` + `gdb_flash_program disable`; without them a *gdb connect*
   probes the QSPI and HardFaults a running target.
5. **Clock is unchanged at 120 MHz.** The port did **not** touch `set_sys_clock_khz`.

## Key files

New:
- `external/FreeRTOS-Kernel` (submodule), `inc/FreeRTOSConfig.h`, `src/freertos_hooks.c`
- `inc/static_task.hpp`, `inc/app_tasks.h`, `src/app_tasks.cpp` — task framework
- `inc/adapter_ctx.h`, `src/adapter_ctx.c` — consolidated cross-core state
- `inc/app_queues.h`, `src/app_queues.c` — `host_tx` + `midi_note` queues
- Debug tooling: `docker/openocd-daemon.sh`, `docker/ocd-client.py`,
  `docker/gdb-dump.py`, `docker/freertos-tasks.py`, `scripts/{dbgd,gdb,ocd}.sh`,
  `docs/DEBUGGING.md`

Heavily changed:
- `src/main.c` — task bodies, `init()`, recovery, the `busy_wait_ms` boot fix
- `src/xbox_controller_driver.c` — IN-error-streak signal, non-blocking `tuh_task_ext`
  pump
- `src/midi.c` — hardware_alarm → FreeRTOS software timer
- `src/{drums,instrument_manager}.c` — queue/`adapter_ctx` wiring
- `CMakeLists.txt`, `cmake/AddBoardTarget.cmake`, `inc/custom_config.h` — kernel wiring,
  `OPT_OS_FREERTOS`, FatFs from vendored tinyusb, `-Og` debug variant

## Build / flash / debug

See [`../BUILDING.md`](../BUILDING.md). Short version:

```sh
scripts/build.sh            # build (build/)
scripts/flash.sh            # flash CUSTOM_REV_0_1 via the dbgd daemon
scripts/uart.sh 40          # read debug UART
scripts/gdb.sh tasks        # per-task backtraces across both cores
scripts/build.sh debug      # -Og deep-backtrace variant -> build-debug/
```

## What was done

In order (each a commit / validated checkpoint on the branch):

1. Build wiring — FreeRTOS-Kernel submodule + import, `FreeRTOSConfig.h`, link Heap4,
   inject kernel onto TinyUSB's include path.
2. Boot the SMP scheduler with one task per core (still `OPT_OS_PICO`).
3. Comms redesign + hazard fixes — `adapter_ctx`, `host_tx`/`midi_note` queues, midi
   software timer; eliminated cross-core host-stack access from core0 and the
   ISR-context fifo write.
4. **Boot deadlock fix** — `sleep_ms` → `busy_wait_ms` pre-scheduler (lesson #1 above).
5. Runtime controller recovery (hub-reset), then **fast IN-error-streak detection**.
6. Debug daemon + dual-core gdb tooling + FreeRTOS task-walker + `-Og` variant.
7. FatFs decoupled from pico-sdk's bundled tinyusb.
8. **`OPT_OS_FREERTOS` flip.**
9. **Split the core0 superloop into the per-concern tasks** above.

## What's left

Phase 5 — tuning/hardening, deliberate and best done with physical-hardware testing.
None of it blocks normal operation.

1. **PIO-USB IRQ-priority hardening.** The recommended SMP knob is to set the PIO-USB
   IRQ to the highest hardware priority so it preempts the FreeRTOS tick/PendSV. The
   port works reliably without it today, so do this as a measured before/after on
   enumeration reliability, not blindly.
2. **Right-size task stacks** via `uxTaskGetStackHighWaterMark` (current sizes are
   conservative estimates; no stack-overflow hook has fired, and
   `configCHECK_FOR_STACK_OVERFLOW=2` is active).
3. **Ship cleanup** — drop the dev-only `configCHECK_FOR_STACK_OVERFLOW` and
   malloc-failed hook for a release build.
4. **Known minor races** (pre-existing, low impact, surfaced in review):
   - `drum_state.input_pkt` is written by core0 (`drum_task`) and core1
     (`tuh_midi_mount_cb`/`umount_cb` via `connect/disconnect_instrument`) — a data race
     on plug/unplug during play. Cleanest fix: route mount/umount through a queue to
     core0 like `midi_note`.
   - `connected_instruments[]` check-then-set is a non-atomic cross-core RMW
     (`src/instrument_manager.c`).
5. **Recovery polish (optional):** a wedge sometimes uses 2 hub-reset attempts because
   the retry budget refills on *received input*, not on mount — bounded and harmless,
   but could refill on a clean mount instead.
6. **Reliability baseline:** assess controller enumeration success across many *physical*
   power cycles vs. the pre-port firmware (SWD resets are not representative — see the
   hardware caveats in [`../../docs/usb-stack-saga.md`](../../docs/usb-stack-saga.md)).
