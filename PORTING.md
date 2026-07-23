# Host-stack port to pico-sdk 2.2.0 / TinyUSB 0.18 / Pico-PIO-USB 0.7.2

> **HISTORICAL — predates the FreeRTOS SMP port.** This doc records the earlier
> dual-core bare-metal *superloop* port. The firmware has since moved to a FreeRTOS
> SMP task model; **[`docs/FREERTOS-PORT.md`](docs/FREERTOS-PORT.md) is the current
> architecture and supersedes the architecture described here.** Several specifics
> below are now false and are flagged inline:
> - core1 is **no longer** launched via `multicore_launch_core1`/`launch_core1_robust`
>   — the FreeRTOS SMP scheduler (`vTaskStartScheduler()`) launches it. ROOT CAUSE #1's
>   early-launch race is now owned by the FreeRTOS port (and its pre-scheduler
>   `sleep_ms` deadlock is that hazard's new incarnation — FREERTOS-PORT.md lesson #1).
> - the dual-core **superloop is gone**, replaced by per-concern FreeRTOS tasks.
> - the operating point is **120 MHz**, not 240 MHz. The 240 MHz overclock that
>   ROOT CAUSE #2 prescribes was later dropped by upgrading pico_pio_usb to upstream
>   `main` (see [`../docs/usb-stack-saga.md`](../docs/usb-stack-saga.md)); the FreeRTOS
>   port did not touch `set_sys_clock_khz`, which is `120000`.
>
> The ROOT CAUSE diagnostics below are kept as historical record — they're still
> valuable — with notes on how each maps to the current world.

Status: **the Xbox controller now enumerates, mounts, and streams input through
the CH334R hub on real hardware.** Both prior blockers are fixed. The controller
reaches `Set Address`, its device descriptor is read, the XBOXH driver claims its
IN endpoint, and input reports flow (`on EP 82 with 8 bytes: OK` every frame).
Xbox auth (`STATE_RUNNING`, AUTH LED) requires a real console and is untested here.

The dependency bump (pico-sdk 1.5.1→2.2.0, TinyUSB 0.16→0.18, Pico-PIO-USB
0.5.3→0.7.2, usb_midi_host→2.0.0) changed several core1-sensitive behaviours.

## What works (verified on hardware)
- Build (all targets), USB device enumeration on the PC.
- SWD flash + debug UART (dlog → uart1 GPIO24/25 → probe `/dev/ttyACM0`).
- **core1 launches and stays up** (root cause #1 — see below).
- USB host SOF runs (`sof_count` climbs ~1000/s).
- **The CH334R hub fully enumerates** (device + config descriptors, addr 5, all 4
  ports powered, port-1 reset).
- **The controller behind the hub enumerates + mounts + streams input** (root
  cause #2 — see below), reliably at 240 MHz. *(Historical: 240 MHz was later
  dropped; the firmware now runs at 120 MHz — see the top-of-file note.)*

## ROOT CAUSE #1 — core1 early-launch handshake race (FIXED)
> **Now owned by the FreeRTOS port.** core1 is no longer launched with
> `multicore_launch_core1`/`launch_core1_robust`; the FreeRTOS SMP scheduler launches
> it inside `vTaskStartScheduler()`, so the manual reset+settle dance below no longer
> exists. The same *early-launch* hazard re-surfaced as a pre-scheduler `sleep_ms`
> boot deadlock — see FREERTOS-PORT.md lesson #1.

**Symptom:** core1 ran ~25–50 ms after `multicore_launch_core1()` then fell back
into the bootrom (PC=0x184, bootrom SP). core0 unaffected. Reproduced even with
core1 reduced to a bare RAM loop and core0 reduced to `set_sys_clock` + launch —
so it was **not** USB/alarm/PSM/watchdog. A `multicore_reset_core1()`+relaunch
revived core1 and it then ran stably.

**Cause:** documented early-launch race in `multicore_launch_core1()` — if core0
launches before core1 has settled into the bootrom wait-for-vector loop, the FIFO
trampoline handshake completes "dirtily" and core1 later falls back to the
bootrom. The pico-sdk 1.5.1→2.2.0 boot-timing change exposed a latent race the
old stack happened to avoid. (RPi forum t=347097, t=303278; Pico-PIO-USB host
example uses the mitigation below.)

**Fix (in `src/main.c`):** match the Pico-PIO-USB example ordering —
```
set_sys_clock_khz(120000, true);
sleep_ms(10);            // <-- the missing piece: let core1 settle
multicore_reset_core1();
multicore_launch_core1(core1_main);
```
plus a ~10 ms settle at the top of `core1_main` before `configure_host()`. See
`launch_core1_robust()`. Verified: core1 no longer dies, SOF runs, hub enumerates.

## ROOT CAUSE #2 — PIO-USB timing margin through the hub repeater (FIXED)
> **The 240 MHz fix prescribed here was later superseded.** Upgrading pico_pio_usb to
> upstream `main` (post-0.7.2 bus-turnaround/handshake timing fixes) made 120 MHz
> reliable, so the overclock was dropped. The firmware — including after the FreeRTOS
> port — runs at **120 MHz** (`set_sys_clock_khz(120000, true)`). See
> [`../docs/usb-stack-saga.md`](../docs/usb-stack-saga.md) for the resolution.

**Symptom:** after the hub resets its downstream port and `USBH Device Attach`
fires, the controller's very first `GET_DESCRIPTOR` SETUP (addr 0, behind the hub)
intermittently got **no handshake at all** — `wait_handshake()` returned 0,
`pio_usb_bus_wait_for_rx_start()` never saw RX-start. All 3 enum attempts then
failed → `Control FAILED` → controller never mounted, `adapter_state` stuck at
`STATE_INIT`. Failure was all-or-nothing per boot (~90% of boots at 120 MHz).

**Diagnosis (on hardware, via dlog instrumentation):**
- Transfers to the **hub itself** (addr 5, 0 repeater hops) were 100% reliable;
  only the device **one hop through the hub's repeater** failed.
- Widening the FS RX-start window 3 µs → 20 µs did **not** help (still
  `start0`/no-response), ruling out a late-turnaround / RX-timeout-window cause.
- Reverting the PR #164 FS inter-packet/turnaround optimizations (commits
  `76a5c1a`, `7f1eaa5`) did **not** help either.
- The failure was a *total non-response*, i.e. the bit-banged PIO-USB packet
  wasn't surviving the CH334R repeater cleanly (marginal edge/bit timing).

**Cause:** Pico-PIO-USB's software-timed USB signalling, at the documented 120 MHz
sys clock, has too little sub-bit timing resolution to push a clean packet through
the extra repeater hop. (The hub itself, directly on the root port, is fine.)

**Fix (in `src/main.c`):** run the RP2040 at **240 MHz** instead of 120 —
`set_sys_clock_khz(240000, true)`. This doubles the PIO sub-bit resolution; the
PIO-USB clock dividers derive from `clk_sys` automatically. The controller then
enumerates reliably (≥95% of rapid SWD-reset boots; the device stack and serial
MIDI are unaffected). 240 MHz is the operating point the Pico-PIO-USB examples use.

**Known residual (~5%, deferred):** a small fraction of *rapid-reset* boots still
fail the addr-0 SETUP on all 3 attempts and leave the controller stuck, because
TinyUSB `process_enumeration()` (`usbh.c`) retries the same GET_DESCRIPTOR without
re-resetting the hub port. A future hardening pass could re-reset the port and
retry on full enumeration failure. The rapid-reset cadence (RP2040 reset every 3 s
while hub+controller stay powered) likely overstates the real single-power-on rate.

## Fixes/patches already applied (this branch)
1. **FIFO mutex deadlock (fixed).** TinyUSB 0.18 made the `osal_pico` mutex a real
   blocking mutex. `src/packet_queue.c` drops the xbox FIFO's redundant read mutex
   (`CREATE_GENERIC_FIFO(... /*rd_mtx=*/false, /*wr_mtx=*/true)`).
2. **core1 launch settle delay** — ROOT CAUSE #1 fix above (`src/main.c`).
3. **240 MHz sys clock** — ROOT CAUSE #2 fix above (`src/main.c`,
   `set_sys_clock_khz(240000, true)`).
4. **`tusb_time_delay_ms_api` override (`src/main.c`)** — lock-free `timerawl`
   busy-wait; TinyUSB 0.18 enum delays use it and `sleep_ms`/`busy_wait` take a
   spin lock that hangs on the PIO-USB core.
5. **Deferred logger (`src/dlog.c`/`inc/dlog.h`)** — core1-safe SPSC RAM ring
   buffer; core1 formats, core0 drains to uart1. TinyUSB host logs routed via
   `CFG_TUSB_DEBUG_PRINTF=dlog_printf`. Made the host debuggable.
6. **Submodule patches** (`patches/`, also applied to checked-out submodules):
   - `patches/tinyusb-0.18-hub-descriptor.patch` — hub `GET_DESCRIPTOR`
     `wValue=0x2900, wLength=8` (CH334R needs the descriptor type in wValue).
     **Confirmed working** — the hub now enumerates.
   - `patches/pico-pio-usb-0.7.2-pid-mismatch.patch` — accept DATA0/1
     PID-mismatched IN packets instead of dropping (re-syncs the data toggle).

Debug scaffolding from the root-cause-#1 hunt has been stripped; `src/main.c`,
`pio_usb.c`, and `pio_usb_host.c` carry only the fixes above (verify with
`git diff <tag>` against the submodule tags — `pio_usb.c` should be empty,
`pio_usb_host.c` should show only the PID-mismatch hunk).

## How to build / flash / observe
All of this is wrapped by the `scripts/` helpers over `docker-compose.yml` — see
[`BUILDING.md`](BUILDING.md). In short: `scripts/build.sh`, `scripts/flash.sh`,
`scripts/reset.sh`, `scripts/uart.sh`.

Two details worth knowing here:
- **Flash halts both cores first** — core1 otherwise interferes with the flash
  algorithm (it runs the USB host loop). The `dbg` service's openocd invocation does
  `targets rp2040.core1; halt; targets rp2040.core0; halt` before `program`.
- If flashing returns `Unknown flash device (ID 0x00ffffff)`, the QSPI flash is wedged
  in continuous-read/QPI mode; SWD cannot reset the external chip — recover with a
  **power-cycle** or hold **BOOTSEL** while plugging in.

The `monitor` compose service owns the debug UART and timestamps it into `.mon/uart.log`
(surviving target resets), so observing never perturbs SWD timing — `scripts/reset.sh`
marks the log and `scripts/uart.sh` reads it.

## Next steps
1. **End-to-end with a real Xbox console:** confirm the device side still
   enumerates at 240 MHz and that auth completes (`adapter_state == STATE_RUNNING`,
   AUTH LED on). Auth cannot be exercised on the bench (no console).
2. **(Deferred) close the ~5% stuck-boot tail:** on full device-behind-hub
   enumeration failure, re-reset the hub port and retry instead of giving up
   (`process_enumeration()` in `usbh.c`).
3. Fold the two submodule patches into the `delabrcd/*` forks.
