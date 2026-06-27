# Building & debugging openrb-pico

Quick reference for working on the RP2040 firmware. Everything runs in Docker via
[`docker-compose.yml`](docker-compose.yml), so the only host requirement is Docker
(Compose v2) with the SWD probe + debug UART plugged in. Helper scripts in
[`scripts/`](scripts/) wrap the compose commands — prefer them over driving compose or
`openocd` by hand.

- Deep porting writeup (core1 race, the 240 MHz fix, etc.): [`PORTING.md`](PORTING.md).
- Why the submodule patches / clock exist, and the **hardware testing caveats**:
  [`../docs/usb-stack-saga.md`](../docs/usb-stack-saga.md) — read this before trusting
  any enumeration A/B result.

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
| `dbg` | one-shot | privileged + `/dev` | openocd flash / reset over SWD |
| `monitor` | long-running (`restart: unless-stopped`) | privileged + `/dev` | owns the debug UART → `.mon/uart.log` |

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
| `build.sh [clean]` | submodules → apply patches → cmake configure (cached) → build. `clean` wipes `build/` first. |
| `apply-patches.sh` | idempotently applies the two submodule patches (`patches/`). Called by `build.sh`. |
| `flash.sh [BOARD]` | halts both cores, then `program …verify reset` of `build/openrb-pico_<BOARD>.elf`. |
| `reset.sh` | marks the UART log, then SWD `reset run`. |
| `uart.sh [N]` | read the monitor log: no arg follows it live; `N` prints the last N lines. |
| `monitor.sh {start\|stop\|restart\|rebuild\|status\|logs}` | `monitor` service lifecycle. Rarely needed — it auto-starts. `rebuild` picks up Dockerfile changes. |
| `common.sh` | shared compose helpers, sourced by the others (env overrides: `ORB_BOARD`, `ORB_TTY`, `ORB_BAUD`). |

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

How it works (`src/usb_log.c`, FatFs from the SDK's TinyUSB at `lib/fatfs`):

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
  if you hotplug the probe, `scripts/monitor.sh restart` re-opens the tty. A one-shot
  `flash`/`reset` always sees the current `/dev` since it's a fresh container.
