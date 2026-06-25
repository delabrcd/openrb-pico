# Host-stack port to pico-sdk 2.2.0 / TinyUSB 0.18 / Pico-PIO-USB 0.7.2

Status: **in progress.** The firmware builds, the USB **device** side works, and the
USB **host** stack now initializes and detects the hub — but enumerating a device
**through** the CH334R hub does not complete yet. This file captures the journey,
the fixes already in place, and the exact next blocker.

The dependency bump (pico-sdk 1.5.1→2.2.0, TinyUSB 0.16→0.18, Pico-PIO-USB
0.5.3→0.7.2, usb_midi_host→2.0.0) changed several core1-sensitive behaviours, so the
host stack needs porting rather than just the old patches.

## What works
- Build (all targets), device enumeration on the PC (PDP/Rock Band adapter).
- SWD flash + debug UART via the Raspberry Pi Debug Probe.
- `tuh_init` completes, all host class drivers init (`MIDIH/XBOXH/HID/HUB`).
- The PIO-USB root port comes up: `pio_usb_root_port[0]` → `initialized=1, connected=1,
  is_fullspeed=1, pin_dp=16, pin_dm=17` (the hub is electrically detected).
- Enumeration **starts**: TinyUSB logs `[1:] USBH Device Attach`.

## What's broken (the current blocker)
Enumeration stalls in `enum_new_device()` (TinyUSB `src/host/usbh.c`) **right after
`hcd_port_reset()`** — i.e. the Pico-PIO-USB bus reset. core1 does not survive into the
post-reset delay (`tusb_time_delay_ms_api`); the next instrumented line never runs and
no device descriptor is ever read. `_usbh_devices[]` stays empty, `adapter_state` stays
`STATE_INIT`, and the xbox controller never mounts (`_xbox_itf[0].daddr == 0`).

The RP2040 system timer **is** ticking globally (verified over SWD), so this is core1's
execution state being corrupted around the PIO `hcd_port_reset` / `pio_usb_host_port_reset_*`
path in Pico-PIO-USB 0.7.2 — not a frozen clock.

## Fixes already applied (in this branch)
1. **FIFO mutex deadlock (fixed, verified).** TinyUSB 0.18 made the `osal_pico` mutex a
   real *blocking* pico mutex (it was a no-op in 0.16). The xbox FIFO's redundant read
   mutex then deadlocked core0 forever. `src/packet_queue.c` now uses
   `CREATE_GENERIC_FIFO(xbox, ..., /*rd_mtx=*/false, /*wr_mtx=*/true)` — single reader
   (core0) needs no read mutex; writers (both cores) keep the write mutex.

2. **Deferred logger (`src/dlog.c`, `inc/dlog.h`).** Blocking `printf` on core1 starves
   the timing-critical PIO-USB SOF interrupt and crashes the host stack — which made the
   host look totally dead under any logging. `dlog` is a lock-free SPSC RAM ring buffer:
   core1 formats into RAM (fast, no UART/mutex), core0 drains it to the debug UART from the
   main loop (`dlog_drain()` in `main()`). TinyUSB host logs are routed to it via
   `CFG_TUSB_DEBUG_PRINTF=dlog_printf` in `inc/custom_config.h`. **This is what made the
   host debuggable at all** — keep it.

3. **`tusb_time_delay_ms_api` override (`src/main.c`).** TinyUSB 0.18's enumeration calls
   `osal_task_delay()` → `sleep_ms()` on the host core; `sleep_ms`/`time_us_64`/`busy_wait`
   all take a spin lock / wait on an alarm that hangs on the core running Pico-PIO-USB. The
   override busy-waits on the raw `timer_hw->timerawl` (lock-free). This is necessary but
   **not sufficient** — the stall is now *before* the delay returns (see blocker above).

4. **Submodule patches** (saved in `patches/`, also applied to the checked-out submodules):
   - `patches/tinyusb-0.18-hub-descriptor.patch` — hub `GET_DESCRIPTOR` `wValue=0x2900`,
     `wLength=8` (CH334R needs the descriptor type in wValue). Apply in
     `external/pico-sdk/lib/tinyusb`.
   - `patches/pico-pio-usb-0.7.2-pid-mismatch.patch` — accept DATA0/1 PID-mismatched IN
     packets instead of dropping them. Apply in `external/Pico-PIO-USB`.
   These are needed once enumeration proceeds; they are **not** the current blocker.
   They must eventually live in the `delabrcd/*` forks (or stay applied as patches).

## Debug build toggles (`inc/custom_config.h`)
- `CFG_TUSB_DEBUG 2` + `CFG_TUH_LOG_LEVEL 2` → host enumeration logs via `dlog`.
  Set both to `0` (and drop `CFG_TUSB_DEBUG_PRINTF`) for a production build.
- Level 3 currently fails to compile (device-side `-Werror` on an unused log string in
  `usbd.c`); level 2 is enough for enumeration tracing.

## How to reproduce / debug (host has no local toolchain; everything runs in Docker)
Build:
```
docker run --rm -v $PWD:/work:z -w /work openrb-pico-dev \
  bash -lc 'cmake --build build -j"$(nproc)"'
```
Flash via the SWD probe + capture the debug UART (which is the probe's `U` port → /dev/ttyACM0):
```
docker run --rm --privileged -v /dev/bus/usb:/dev/bus/usb --device /dev/ttyACM0 \
  -v $PWD:/work:z -w /work ubuntu:24.04 bash -c '
    apt-get update -qq && apt-get install -y -qq openocd >/dev/null
    openocd -f interface/cmsis-dap.cfg -c "adapter speed 2000" -f target/rp2040.cfg \
      -c "program build/openrb-pico_CUSTOM_REV_0_1.elf verify reset exit"
    stty -F /dev/ttyACM0 115200 raw -echo
    ( timeout 12 cat /dev/ttyACM0 ) &
    sleep 1
    openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "init; reset run; shutdown"
    wait'
```
GDB inspection (openocd gdb server on :3333 core0, :3334 core1):
```
gdb-multiarch -q build/openrb-pico_CUSTOM_REV_0_1.elf \
  -ex "target extended-remote localhost:3333" -ex "monitor halt" \
  -ex "print pio_usb_root_port[0]" -ex "print _xbox_itf[0].daddr" -ex "print adapter_state"
```
To trace the stall, re-add `dlog_printf` markers around each step of `enum_new_device`
in `external/pico-sdk/lib/tinyusb/src/host/usbh.c` (the `hub_addr==0` branch:
`hcd_port_reset` → delay → `hcd_port_reset_end` → debounce → `hcd_port_connect_status`
→ `hcd_port_speed_get`). Last build observed: reaches just after `hcd_port_reset`, dies
before the post-reset delay returns.

## Next steps
1. Get core1's PC/backtrace at the moment of the stall (core1 gdb on :3334 was flaky;
   try halting both cores from :3333 and reading core1 via `monitor`/`mdw`, or set a
   breakpoint on `hcd_port_reset_end`). Determine: hard-fault vs spin vs spin-lock wait.
2. Audit `pio_usb_host_port_reset_start/end` (Pico-PIO-USB 0.7.2) — what it does to core1's
   PIO/IRQ/clock state; compare to the 0.5.3 version the firmware originally used.
3. Check for spin-lock exhaustion/conflict: Pico-PIO-USB critical sections + TinyUSB
   `osal_queue` `critical_section_init` + pico-sdk timer all claim hardware spin locks.
4. Once enumeration proceeds, validate the hub + PID patches actually carry the controller
   through to `xboxh_mount_cb` ("Controller Connected") and Xbox auth (AUTH LED / `adapter_state == STATE_RUNNING`).
