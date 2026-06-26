# Host-stack port to pico-sdk 2.2.0 / TinyUSB 0.18 / Pico-PIO-USB 0.7.2

Status: **host stack now brings up and enumerates the hub on real hardware.** The
core1 blocker (below) is fixed; the remaining issue is that a device **behind the
CH334R hub** (the Xbox controller) does not complete its control transfers.

The dependency bump (pico-sdk 1.5.1→2.2.0, TinyUSB 0.16→0.18, Pico-PIO-USB
0.5.3→0.7.2, usb_midi_host→2.0.0) changed several core1-sensitive behaviours.

## What works (verified on hardware, rev 0.2 board)
- Build (all targets), USB device enumeration on the PC.
- SWD flash + debug UART (dlog → uart1 GPIO24/25 → probe `/dev/ttyACM0`).
- **core1 launches and stays up** (was the headline blocker — see below).
- USB host SOF runs (`sof_count` climbs ~1000/s).
- **The CH334R hub fully enumerates**: device + config descriptors read, address
  set (addr 5), all 4 ports powered, port-1 connection detected + reset, and
  `[1:] USBH Device Attach` fires for the controller behind it.

## ROOT CAUSE #1 — core1 early-launch handshake race (FIXED)
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

## CURRENT BLOCKER #2 — control transfer to a device behind the hub times out
After the hub resets its downstream port and `USBH Device Attach` fires, the very
first `GET_DESCRIPTOR` (8 bytes) to the controller (addr 0, behind the hub) fails:
`on EP 00 with 0 bytes: FAILED` → "Enumeration attempt 1/2/3" all fail → the
controller never mounts (`xboxh_mount_cb` never runs, `adapter_state` stays
`STATE_INIT`).

Key contrast: transfers to the **hub itself** (addr 5, directly on the root port)
all succeed; only transfers to the device **one hop through the hub's repeater**
fail. In `usb_in_transaction()` (Pico-PIO-USB `pio_usb_host.c`) it takes the
`res = -1` path — `pio_usb_bus_receive_packet_and_handshake()` returns < 0 and the
PID is not NAK/STALL, i.e. a **receive timeout/garble**, retried 3× then
`ENDPOINT_ERROR`. Most likely a **PIO-USB 0.7.2 RX-timing margin** that the extra
hub-hop latency pushes the response outside of (0.5.3 tolerated it). Needs
investigation in `pio_usb_bus.c` RX timing / `pio_usb_bus_receive_packet_and_handshake`.

## Fixes/patches already applied (this branch)
1. **FIFO mutex deadlock (fixed).** TinyUSB 0.18 made the `osal_pico` mutex a real
   blocking mutex. `src/packet_queue.c` drops the xbox FIFO's redundant read mutex
   (`CREATE_GENERIC_FIFO(... /*rd_mtx=*/false, /*wr_mtx=*/true)`).
2. **core1 launch settle delay** — ROOT CAUSE #1 fix above (`src/main.c`).
3. **`tusb_time_delay_ms_api` override (`src/main.c`)** — lock-free `timerawl`
   busy-wait; TinyUSB 0.18 enum delays use it and `sleep_ms`/`busy_wait` take a
   spin lock that hangs on the PIO-USB core.
4. **Deferred logger (`src/dlog.c`/`inc/dlog.h`)** — core1-safe SPSC RAM ring
   buffer; core1 formats, core0 drains to uart1. TinyUSB host logs routed via
   `CFG_TUSB_DEBUG_PRINTF=dlog_printf`. Made the host debuggable.
5. **Submodule patches** (`patches/`, also applied to checked-out submodules):
   - `patches/tinyusb-0.18-hub-descriptor.patch` — hub `GET_DESCRIPTOR`
     `wValue=0x2900, wLength=8` (CH334R needs the descriptor type in wValue).
     **Confirmed working** — the hub now enumerates.
   - `patches/pico-pio-usb-0.7.2-pid-mismatch.patch` — accept DATA0/1
     PID-mismatched IN packets instead of dropping. Applied; not sufficient for
     blocker #2 (that is a receive *timeout*, not a PID mismatch).

## Debug instrumentation currently in the tree (REMOVE before merge)
`src/main.c` and `external/Pico-PIO-USB/src/pio_usb_host.c` carry a lot of
diagnostic scaffolding used to find root cause #1: `g_c1_heartbeat`/`g_c1_time`/
`g_c0_heartbeat`/`g_delay_*`/`g_relaunch_*` probes, a RAM-resident `isr_hardfault`
capture, `g_sof_enter/exit`/`g_frame_stage` markers, the `ORB_*` test toggles, and
the dlog liveness beacon in `main()`. These should be stripped once blocker #2 is
fixed, keeping only: the FIFO fix, the core1 settle delay, the `tusb_time_delay_ms_api`
override, dlog (optional), and the two submodule patches.

## How to build / flash / observe (everything in Docker)
Build:
```
docker run --rm -v $PWD:/work:z -w /work openrb-pico-dev \
  bash -lc 'cmake --build build -j"$(nproc)"'
```
Persistent debug container (toolchain pre-installed; avoids per-run apt):
```
docker run -d --name orb-dbg --privileged -v /dev/bus/usb:/dev/bus/usb -v /dev:/dev \
  -v $PWD:/work:z -w /work ubuntu:24.04 sleep infinity
docker exec orb-dbg bash -c 'apt-get update -qq && apt-get install -y -qq openocd gdb-multiarch binutils-arm-none-eabi'
```
Flash (halt both cores first — avoids core1 interfering with the flash algorithm):
```
docker exec orb-dbg bash -c 'cd /work && openocd -f interface/cmsis-dap.cfg \
  -c "adapter speed 4000" -f target/rp2040.cfg \
  -c "init" -c "targets rp2040.core1" -c "catch {halt}" -c "targets rp2040.core0" -c "catch {halt}" \
  -c "program build/openrb-pico_CUSTOM_REV_0_1.elf verify reset exit"'
```
Observe autonomously over the debug UART (no SWD — does not perturb timing):
```
docker exec orb-dbg bash -c 'stty -F /dev/ttyACM0 115200 raw -echo; timeout 10 cat /dev/ttyACM0'
```
If flashing returns `Unknown flash device (ID 0x00ffffff)`, the QSPI flash is
wedged in continuous-read/QPI mode; SWD cannot reset the external chip — recover
with a **power-cycle** or hold **BOOTSEL** while plugging in.

## Next steps
1. **Blocker #2:** instrument/measure the device-behind-hub IN transaction.
   Determine res=-1 vs res=-2 (RX-complete-but-bad vs no-RX) on the controller's
   first `GET_DESCRIPTOR`; compare the RX timing window in Pico-PIO-USB 0.7.2's
   `pio_usb_bus_receive_packet_and_handshake`/`pio_usb_bus.c` against 0.5.3. Also
   try a longer post-reset settle and verify the controller's reported speed.
2. Once the controller mounts (`xboxh_mount_cb` → "Controller Connected") and Xbox
   auth completes (`adapter_state == STATE_RUNNING`, AUTH LED), strip the debug
   instrumentation (see above) and fold the submodule patches into the `delabrcd/*`
   forks.
