# Building & debugging openrb-pico

Quick reference for working on the RP2040 firmware. Everything runs in Docker via
[`docker-compose.yml`](docker-compose.yml), so the only host requirement is Docker
(Compose v2) with the SWD probe + debug UART plugged in. Helper scripts in
[`scripts/`](scripts/) wrap the compose commands — prefer them over driving compose or
`openocd` by hand.

- **Current architecture / handoff** — the firmware is now a **FreeRTOS SMP** build
  (core1 = the sole PIO-USB host task; core0 = USB-device + feature tasks):
  [`docs/FREERTOS-PORT.md`](docs/FREERTOS-PORT.md). Read this first;
  it and `DEBUGGING.md` carry the detail.
- On-target debugging — the persistent `dbgd` daemon, **dual-core backtraces**, and
  FreeRTOS thread awareness: [`docs/DEBUGGING.md`](docs/DEBUGGING.md).
- Deep porting writeup (core1 race, the original clock/PIO-USB work): [`PORTING.md`](PORTING.md).
  Pre-FreeRTOS history — the firmware runs at **120 MHz** (unchanged by the FreeRTOS port).
- Why the submodule patches / clock exist, and the **hardware testing caveats**:
  [`../docs/usb-stack-saga.md`](../docs/usb-stack-saga.md) — read this before trusting
  any enumeration A/B result.
- How the controller is brought back after it wedges or a warm reset (runtime hub-reset
  recovery without a reboot, with heartbeat-keyed auto-reboot as last resort, and why
  it's a hardware limitation): [`docs/warm-reset-recovery.md`](docs/warm-reset-recovery.md).

## Prerequisites

- **Docker + Compose v2.** A single image (`openrb-pico-dev`, built from
  [`.devcontainer/Dockerfile`](.devcontainer/Dockerfile)) carries both the cross
  compiler and the debug tools (openocd, gdb, gawk). Compose builds it on first use.
- **Submodules** checked out (`git submodule update --init --recursive`) — `build.sh`
  does this if `external/pico-sdk` is missing.
- **A CMSIS-DAP probe** wired to the target's SWD pins, plus the debug UART
  (dlog → uart1 GPIO24/25), enumerating on the host as `/dev/ttyACM0`.

The compose file defines three services off that one image (see
[`docker-compose.yml`](docker-compose.yml)):

| service | lifetime | privilege | role |
|---------|----------|-----------|------|
| `build` | one-shot | unprivileged | cross-compile the firmware |
| `dbg` | one-shot | privileged + `/dev` | one-shot openocd (manual use; flash/reset now route through `dbgd`) |
| `monitor` | long-running (`restart: unless-stopped`) | privileged + `/dev` | owns the debug UART → `.mon/uart.log` |
| `dbgd` | long-running (`restart: unless-stopped`) | privileged + `/dev` | owns the SWD probe: one persistent openocd (gdbservers :3333/:3334, command ports :4444/:6666). See [`docs/DEBUGGING.md`](docs/DEBUGGING.md) |

## Common workflow

```sh
scripts/build.sh             # configure (first time) + build all targets
scripts/flash.sh             # flash the default board (CUSTOM_REV_0_1); also resets+runs
scripts/uart.sh 40           # read the last 40 lines of debug UART
```

The UART is captured by the **`monitor` service**: it owns the tty and appends
(timestamped) to `.mon/uart.log`, auto-starting whenever any script touches the debug
side and reconnecting across resets. So there's no capture to time — just read the log.
To capture a clean **boot**, reset and then read:

```sh
scripts/reset.sh             # writes a "==== RESET ... ====" marker, then resets
scripts/uart.sh 40           # boot output is already in the log
```

A healthy boot ends with the Xbox One handshake flowing — `IN (CMD_POWER_MODE)`,
`CMD_LED_MODE`, `CMD_AUTHENTICATE` — i.e. the controller enumerated through the hub:

```
==================== RESET 2026-06-27T17:00:32Z ====================
[17:00:32.550]  openrb debug console initialized...
...
[17:00:33.070] IN (CMD_POWER_MODE): 05 20 00 01 00
[17:00:33.073] IN (CMD_LED_MODE): 0A 20 01 03 00 01 14
[17:00:33.077] IN (CMD_AUTHENTICATE): 06 20 02 02 01 00
```

Board targets are `CUSTOM_REV_0_1` (default) and `FEATHER`. Override the default for
flash/reset via the `ORB_BOARD` env var or a positional arg: `scripts/flash.sh FEATHER`.

## Scripts

| script | what it does |
|--------|--------------|
| `build.sh [debug] [clean]` | submodules → cmake configure (cached) → build. `clean` wipes the tree first; `debug` builds the `-Og` deep-backtrace variant in a separate `build-debug/` (see below). |
| `flash.sh [BOARD]` | halts both cores, then `program …verify reset` of `build/openrb-pico_<BOARD>.elf` — **via the `dbgd` daemon** (no second openocd). `ORB_BUILD_DIR=build-debug` flashes the debug ELF. |
| `reset.sh` | marks the UART log, then SWD `reset run` **via the `dbgd` daemon**. |
| `gdb.sh [--no-resume] [bt\|regs\|tasks\|core0 "<cmd>"\|core1 "<cmd>"]` | **dual-core backtraces** from both gdbservers (and `tasks` = all FreeRTOS tasks), then resume. `ORB_BUILD_DIR=build-debug` uses the debug ELF. See [`docs/DEBUGGING.md`](docs/DEBUGGING.md). |
| `ocd.sh '<tcl>'` | send any openocd/TCL command to the running `dbgd` daemon (e.g. `scripts/ocd.sh 'targets'`). |
| `uart.sh [N]` | read the monitor log: no arg follows it live; `N` prints the last N lines. |
| `monitor.sh {start\|stop\|restart\|rebuild\|status\|logs}` | `monitor` service lifecycle. Rarely needed — it auto-starts. `rebuild` picks up Dockerfile changes. |
| `dbgd.sh {start\|stop\|restart\|rebuild\|status\|logs}` | `dbgd` (SWD debug daemon) lifecycle. Rarely needed — it auto-starts. |
| `common.sh` | shared compose helpers, sourced by the others (env overrides: `ORB_BOARD`, `ORB_TTY`, `ORB_BAUD`, `ORB_ADAPTER_SPEED`). |

## Deep-backtrace debug build (`-Og`)

The default build ships at the Pico-SDK default optimization (≈`-O2`), which is what we
test for timing — but it makes gdb backtraces **shallow** (the optimizer omits frame
pointers and inlines, so the unwinder loses frames). When you need to see *deep*
multi-frame backtraces (e.g. chasing a hang), build the opt-in `-Og` variant:

```sh
scripts/build.sh debug                       # configures + builds in build-debug/ (-DORB_DEBUG_BUILD=ON)
ORB_BUILD_DIR=build-debug scripts/flash.sh   # flash the debug ELF
ORB_BUILD_DIR=build-debug scripts/gdb.sh tasks   # debug-symbol-rich backtraces
```

- `build/` (optimized) and `build-debug/` (`-Og`) **coexist** — no reconfiguring back
  and forth. The default `build/`, `flash.sh`, and `gdb.sh` are unchanged; the debug
  tree is purely opt-in via the `debug` arg / `ORB_BUILD_DIR` env.
- `ORB_DEBUG_BUILD=ON` adds `-Og -g3 -fno-omit-frame-pointer -funwind-tables
  -fasynchronous-unwind-tables` to the **project's own sources only** (`src/*`, via the
  `add_board_target` targets in [`cmake/AddBoardTarget.cmake`](cmake/AddBoardTarget.cmake)).
  The SDK / TinyUSB / FreeRTOS libraries stay at their default optimization — this keeps
  the build fast and the blast radius small; we rarely need to unwind through them.
- **`-Og` slightly changes timing.** It is a debug aid you flash *deliberately* when
  debugging, not what we ship/test. The chip stays at **120 MHz** (the clock is not
  touched). For any timing-sensitive enumeration test, go back to the optimized `build/`.

Build artifacts land in `build/openrb-pico_<BOARD>.{elf,bin,uf2}` (`build*` is gitignored).
The UART log lives at `.mon/uart.log` (also gitignored). After editing
`.devcontainer/Dockerfile`, rebuild the image with `scripts/monitor.sh rebuild` (or
`docker compose build`).

## Logging to a USB flash drive

For tests away from the dev machine (no debug UART attached), the firmware mirrors the
`dlog` stream to a **USB flash drive plugged into the CH334R hub**, written as a normal
FAT file you pull and read on any PC. This replaced an earlier onboard-QSPI approach,
which deadlocked: programming QSPI flash forces XIP off, so neither core can execute
from flash during the op, which fights the timing-critical PIO-USB host. Writing to a
USB drive never touches XIP, so that whole hazard is gone. (See the git history /
`docs/usb-stack-saga.md` for the QSPI saga.)

How it works (`src/usb_log.c`, FatFs sourced from the **vendored** TinyUSB at
`external/tinyusb` — `PICO_TINYUSB_PATH/lib/fatfs/source`, the `FATFS_DIR` in
[`CMakeLists.txt`](CMakeLists.txt), *not* pico-sdk's bundled tinyusb):

- **core0** drains `dlog` and pushes bytes into a lock-free SPSC RAM ring
  (`usb_log_write`, the `dlog` sink) — cheap, non-blocking.
- **core1** owns the USB host stack, so it drains the ring and does the actual MSC
  writes to `LOG.TXT` (`usb_log_task`, called from the core1 loop after `tuh_task`).
  The blocking FatFs disk I/O pumps `tuh_task` while waiting, which keeps the
  controller serviced — see the note below.
- It **appends** (`FA_OPEN_APPEND`), re-mounting/re-opening across the adapter's many
  Xbox-initiated reboots, and `f_sync`s ~1×/s so a pulled drive is readable up to ~1 s
  ago. `usb_log_set_enabled()` is a dormant hook to gate writes (e.g. during auth) —
  unused, since gameplay testing showed no responsiveness impact.

Usage: plug a USB flash drive into the hub alongside the controller; the log streams to
`LOG.TXT` automatically. Pull it and read on any PC. A `[USBLOG] stick mounted …` line
on the debug UART confirms it came up.

Caveats:
- The drive shares the finicky PIO-USB/CH334R bus, so enumeration has the same
  flakiness as the controller — a clean power-cycle is the reliable way to bring up
  both (SWD resets wedge the hub; see Gotchas).
- The in-RAM ring (32 KB) drops oldest on overflow if the drive can't keep up; the
  last unsynced (<~1 s) writes are lost on an abrupt pull.

## Gotchas

- **SWD reset ≠ physical reset.** `reset.sh` does not power-cycle the CH334R hub and is
  not equivalent to the RESET button for USB-wedge behaviour. Don't trust enumeration
  pass/fail from an SWD reset — use a physical reset + the controller LED as ground
  truth. Details in [`../docs/usb-stack-saga.md`](../docs/usb-stack-saga.md).
- **Hub wedge.** After many resets / failed enumerations the hub accumulates bad state
  until a full **power cycle** — nothing enumerates until then. Power-cycle between A/B
  candidates; keep reset batches small.
- **`Unknown flash device (ID 0x00ffffff)` when flashing.** QSPI flash is wedged in
  continuous-read/QPI mode; SWD can't reset the external chip. Power-cycle, or hold
  **BOOTSEL** while plugging in.
- **Probe not visible in the container.** The privileged services pass through `/dev`;
  if you hotplug the **UART**, `scripts/monitor.sh restart` re-opens the tty, and if you
  hotplug the **SWD probe**, `scripts/dbgd.sh restart` re-homes openocd (its inner
  reconnect loop also recovers across probe re-enumeration on its own).
- **One openocd owns the probe.** `flash`/`reset`/`gdb` all route through the single
  persistent `dbgd` daemon — don't run a bare `openocd` against the probe while it's
  up. To use an external debugger, `scripts/dbgd.sh stop` first. See
  [`docs/DEBUGGING.md`](docs/DEBUGGING.md).
