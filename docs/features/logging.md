# Feature: logging overhaul

Status: PROPOSED (spec only — not yet implemented)
Related: [cpp-overhaul](cpp-overhaul.md), [runtime-configurator](runtime-configurator.md)

## Summary

Rework the firmware's debug output into a unified, deferred logging protocol with
log **levels** (ERROR/WARN/INFO/DEBUG/TRACE), **category tags** (HOST, DEV, DRUM,
MIDI, RECOV, USBLOG, …), **timestamps**, and a clean macro API (`LOG_ERR(cat, …)`,
`LOG_INFO(cat, …)`, hex-dump). It preserves the existing deferred, lock-free,
per-core SPSC ring design so it stays safe to call from the timing-critical core1
PIO-USB task. The on-wire line format is `[timestamp][LEVEL][core][CAT] message`,
with optional ANSI color on the UART sink only. The current `OPENRB_DEBUG(...)`
macro and `dlog_printf` keep working through a compatibility shim, so migration is
incremental.

## Motivation

Today's debug output is unstructured and undifferentiated:

- `inc/orb_debug.h` exposes only `OPENRB_DEBUG(...)` (a thin alias for
  `dlog_printf`) and `OPENRB_DEBUG_BUF()`. No levels, no timestamps, no source
  attribution. The single on/off switch is `OPENRB_DEBUG_ENABLED` (`inc/orb_debug.h:4`).
- ~40 call sites across `src/{main,drums,guitar,usb_log,xbox_controller_driver,
  wla_identifiers,xbox_one_protocol}.c` hand-roll prefixes inconsistently — some
  use ad-hoc bracket tags like `"[USBLOG] …"` (`src/usb_log.c:59`), most have no
  prefix at all (`src/drums.c:115` `"NOTE ON: %d %d\r\n"`), and many embed `\r\n`
  by hand.
- There is no way to tell **which core** emitted a line, even though core0 and
  core1 both produce into the same drained stream (`src/dlog.c:46`).
- There is no time reference, so ordering and latency of events (enumeration,
  recovery, note timing) can't be reasoned about from a capture.
- TinyUSB host logs are funnelled through the same untagged `dlog_printf`
  (`inc/custom_config.h:63-65`, `CFG_TUSB_DEBUG_PRINTF dlog_printf`), so they
  interleave indistinguishably with our own lines.
- Filtering requires recompiling (flip `OPENRB_DEBUG_ENABLED`) or grepping the
  capture after the fact.

The goal (project owner): "add timestamps and a unified logging protocol with
proper log levels … maintain the deferred logger, but make it significantly more
featureful and professional looking."

## Goals / Non-goals

**Goals**

- Compile-time minimum level that is **truly zero-cost** when a call is below it
  (no argument evaluation, no code emitted).
- Optional **runtime** level control — at least a global runtime threshold, ideally
  per-category.
- **Category tags** so output is attributable and filterable by subsystem.
- **Timestamps** captured at the log call on the producing core (not at drain).
- A small, consistent macro/API usable identically from **core0 and core1** and
  from TinyUSB's `CFG_TUSB_DEBUG_PRINTF` hook.
- A professional, column-aligned line format; optional ANSI color on UART only.
- Preserve the deferred + lock-free + per-core-SPSC discipline exactly (no locks,
  no `sleep_ms`/`board_millis`/spinlocks on core1 — see
  [`FREERTOS-PORT.md`](../FREERTOS-PORT.md) gotcha #2).
- Per-sink configuration (level, color on/off) for the UART and USB-stick sinks.
- Backward-compatible migration: `OPENRB_DEBUG` keeps working.

**Non-goals**

- No change to the SPSC ring's inter-core safety model or to the
  `housekeeping_task` drain wiring (`src/main.c:650`).
- No new transport/sink beyond the existing UART (`uart1`, GPIO24/25) and the
  USB-stick file (`LOG.TXT`); a future network/RTT sink is out of scope here.
- No structured/binary log protocol for host-side tooling (mentioned only as an
  open question).
- Not a general printf-replacement audit — only the debug/log paths.
- No change to clock, build variants, or task topology.

## Current state (in code)

- **Deferred core** — `src/dlog.c` / `inc/dlog.h`: two lock-free SPSC rings,
  `dlog_buf[DLOG_NRING][DLOG_SIZE]` with `DLOG_NRING = 2` and `DLOG_SIZE = 16384`
  (`src/dlog.c:14-26`). The producing core is selected by `sio_hw->cpuid & 1u`
  (`src/dlog.c:46`); each core writes only its own ring's `head`, core0 drains both
  tails. Aligned 32-bit head/tail loads/stores are atomic on M0+, so no lock is
  needed. A full ring **drops the rest of the message** rather than blocking
  (`src/dlog.c:52`), which protects the core1 USB SOF.
- **Producer** — `dlog_printf` formats into a 160-byte stack buffer with
  `vsnprintf`, then copies bytes into the calling core's ring (`src/dlog.c:38-58`).
  Formatting happens **on the producer core, at call time** today.
- **Consumer/drain** — `dlog_drain()` (`src/dlog.c:60-81`) copies each ring byte to
  `uart_putc_raw(uart1, …)` and, if set, to a secondary sink in 128-byte chunks.
  Called from `housekeeping_task` (`src/main.c:650`) and once more on the
  watchdog-reboot path before reset (`src/main.c:541`).
- **Init / wiring** — `dlog_init()` brings up `uart1` at 115200 on GPIO24/25
  (`src/dlog.c:32-36`, called `src/main.c:562`). The USB-stick mirror is attached
  with `dlog_set_sink(usb_log_write)` (`src/main.c:591`).
- **UART sink** — `uart1`, TX=GPIO24 RX=GPIO25, 115200 (`src/dlog.c:10-12`).
- **File sink** — `src/usb_log.c`: a separate 32 KiB SPSC ring (core0 producer via
  the `dlog` sink callback `usb_log_write`, `src/usb_log.c:24-34`; core1 consumer
  `usb_log_task` does the FatFs `f_write` to `LOG.TXT`, `src/usb_log.c:75-113`).
  It already uses `timer_hw->timerawl` for its periodic flush timer
  (`src/usb_log.c:84,108`).
- **Public macros** — `inc/orb_debug.h`: `OPENRB_DEBUG(...)`,
  `OPENRB_DEBUG_BUF(x,n)` (deferred hex dump via `print_buf`), gated by
  `OPENRB_DEBUG_ENABLED` (`inc/orb_debug.h:14-35`).
- **TinyUSB integration** — `inc/custom_config.h:63-65`: `CFG_TUSB_DEBUG 0`,
  `CFG_TUSB_DEBUG_PRINTF dlog_printf`; host log verbosity `CFG_TUH_LOG_LEVEL 2`
  (`inc/custom_config.h:102`).
- **Timestamp source** — `timer_hw->timerawl` is a lock-free 32-bit (1 MHz) raw
  timer usable on both cores; `board_millis()` is core0-only
  ([`FREERTOS-PORT.md`](../FREERTOS-PORT.md) gotcha #2; usage at
  `src/main.c:78`, `src/usb_log.c:84`).

## Design

### Overview

Keep `dlog`'s per-core SPSC rings as the transport. Add a thin **front end**
(`inc/orb_log.h` + `src/orb_log.c`) that owns levels, categories, the timestamp,
and the line format, and emits finished bytes into the ring via the existing
producer path. The drain and sinks stay as-is, gaining only per-sink level/color
handling.

```
LOG_INFO(HOST, "...")  ──┐
LOG_ERR (DRUM, "...")  ──┼─► orb_log front end (level gate, ts capture, format)
TinyUSB CFG_TUSB_..._PRINTF ─┘        │
                                      ▼
                          dlog ring (per-core SPSC, unchanged)
                                      │  housekeeping_task → dlog_drain()
                              ┌───────┴────────┐
                              ▼                ▼
                         UART sink        USB-stick sink (usb_log → LOG.TXT)
                       (color, level)        (no color, level)
```

### Log levels

Five levels, ordered numerically so comparisons are trivial:

| Value | Name  | Macro       | Use |
|-------|-------|-------------|-----|
| 1 | ERROR | `LOG_ERR`   | faults, failed enumeration, dropped state |
| 2 | WARN  | `LOG_WARN`  | recovery engaged, retries, degraded paths |
| 3 | INFO  | `LOG_INFO`  | lifecycle: mount/umount, announce, auth |
| 4 | DEBUG | `LOG_DBG`   | per-event detail (note on/off, packets) |
| 5 | TRACE | `LOG_TRC`   | high-rate firehose (per-report dumps) |

(0 reserved for "off".)

**Compile-time minimum (zero-cost).** A single `ORB_LOG_LEVEL` define (default
e.g. `LOG_LEVEL_DEBUG`) sets the highest level compiled in. Each macro expands to
nothing when its level exceeds `ORB_LOG_LEVEL`, so arguments are never evaluated
and no code is emitted:

```c
#define LOG_ERR(cat, ...) ORB_LOG_(LOG_LEVEL_ERROR, cat, __VA_ARGS__)
#if ORB_LOG_LEVEL >= LOG_LEVEL_DEBUG
#  define LOG_DBG(cat, ...) ORB_LOG_(LOG_LEVEL_DEBUG, cat, __VA_ARGS__)
#else
#  define LOG_DBG(cat, ...) ((void)0)
#endif
```

This replaces the all-or-nothing `OPENRB_DEBUG_ENABLED`. The legacy switch maps to
`ORB_LOG_LEVEL = 0` (off) vs the default level.

**Optional runtime level.** Above the compile-time floor, a runtime threshold lets
you quiet/verbose a running unit without reflashing. Proposed:
`orb_log_set_level(level)` global, plus per-category override
`orb_log_set_cat_level(cat, level)` backed by a small `uint8_t levels[CAT_COUNT]`
table. The runtime check is one array load + compare in the front end — cheap, and
it touches only a plain `uint8_t` written from core0 (config) and read from both
cores; a stale read merely admits/drops one line, which is benign (no lock needed,
consistent with the existing lock-free philosophy). See open questions for whether
per-category runtime control is worth the table vs a single global runtime level.

### Category / module tags

A compact enum + parallel name table:

```c
typedef enum { CAT_SYS, CAT_HOST, CAT_DEV, CAT_DRUM, CAT_MIDI,
               CAT_RECOV, CAT_USBLOG, CAT_TUSB, CAT_COUNT } orb_cat_t;
static const char *const orb_cat_name[CAT_COUNT] =
    { "SYS","HOST","DEV","DRUM","MIDI","RECOV","USBLOG","TUSB" };
```

Names are fixed-width (≤6 chars) so columns align. The tag is rendered from the
enum at format time. `CAT_TUSB` is the bucket for the TinyUSB hook (see below).
Mapping of existing sites: `usb_log.c` → `USBLOG`, `drums.c`/`xbox_one_protocol.c`
→ `DRUM`/`MIDI`, host driver/recovery in `main.c`/`xbox_controller_driver.c` →
`HOST`/`RECOV`, device/announce/auth → `DEV`, boot/reset-cause → `SYS`.

### Timestamps

- **Source:** `timer_hw->timerawl` — a 1 MHz, lock-free 32-bit raw timer readable
  on both cores (unlike `board_millis()`, core0-only). 32 bits at 1 MHz wraps every
  ~71.6 min, which is more than adequate for a debugging session; the renderer just
  shows the low word.
- **Captured at log-call time on the producing core**, not at drain. This is the
  key deferred-logging requirement: the drain happens later in `housekeeping_task`,
  so a drain-time timestamp would be wrong and would collapse all lines onto the
  drain tick. The front end snapshots `timer_hw->timerawl` immediately on entry,
  before formatting.
- **Format:** seconds.milliseconds with fixed width, e.g. `[  12.345678]` (us)
  or `[  12.345]` (ms) — propose microsecond precision since the source is 1 MHz
  and note timing matters. Rendered as `sec = raw/1000000`, `frac = raw%1000000`.
  Fixed column width keeps alignment.

### Line format

```
[  12.345678][INFO ][C0][HOST  ] controller 1 connected
[  12.350011][ERR  ][C1][USBLOG] f_open 1:LOG.TXT failed (3)
```

`[timestamp][LEVEL][core][CAT] message` — all fixed-width fields:

- timestamp: right-aligned, fixed width (see above).
- LEVEL: 5-char padded (`ERR  `,`WARN `,`INFO `,`DEBUG`,`TRACE`).
- core: `C0`/`C1` from `sio_hw->cpuid & 1u` (same source `dlog` already uses,
  `src/dlog.c:46`).
- CAT: 6-char padded name.
- message: the user's `printf` payload. The front end appends `\r\n`; **call sites
  stop hand-writing `\r\n`** (removes the inconsistency seen across current sites).

**ANSI color (UART sink only).** Per-level SGR color (red ERROR, yellow WARN,
default INFO, dim DEBUG/TRACE) wrapped around the LEVEL field or whole line. Color
must **not** go to the file sink (it would corrupt `LOG.TXT` for later reading).
Two clean options — see "Sinks" and open questions: either (a) the front end never
emits color into the ring and each sink colorizes on output, or (b) color is
emitted into the ring and the file sink strips it. (a) is preferred because the
ring then holds plain text and the file sink does zero work.

### API

```c
// inc/orb_log.h
void orb_log_init(void);                       // wraps dlog_init() + sink setup
void orb_log_set_level(int level);             // global runtime floor
void orb_log_set_cat_level(orb_cat_t, int);    // optional per-category
void orb_log_sink_set_color(/*sink id*/, bool);// UART color on/off
void orb_log_sink_set_level(/*sink id*/, int); // per-sink threshold

#define LOG_ERR(cat, ...)  /* level-gated, see above */
#define LOG_WARN(cat, ...)
#define LOG_INFO(cat, ...)
#define LOG_DBG(cat, ...)
#define LOG_TRC(cat, ...)
#define LOG_HEXDUMP(cat, level, ptr, len)      // deferred hex dump (replaces OPENRB_DEBUG_BUF)
```

Internally `ORB_LOG_(level, cat, fmt, ...)`:
1. compile-time gate (macro expansion),
2. runtime gate (`level <= effective_level(cat)`),
3. snapshot `timer_hw->timerawl`, read `cpuid`,
4. one `vsnprintf` into a stack buffer that **prepends the formatted prefix**
   then the message (single buffer, single ring write — preserves the
   one-`head`-store batch-publish property of `src/dlog.c:56`),
5. push to the calling core's ring via the existing producer logic.

`LOG_HEXDUMP` replaces `OPENRB_DEBUG_BUF`/`print_buf` (`inc/orb_debug.h:14-28`),
keeping the chunk-at-a-time deferred behavior but tagging/leveling the lines.

**TinyUSB hook.** `CFG_TUSB_DEBUG_PRINTF` requires a plain `int (*)(const char*,…)`
and TinyUSB emits its own already-formatted, multi-call fragments (no level/cat).
Keep a shim `int orb_log_tusb_printf(const char *fmt, ...)` that tags the output
`CAT_TUSB` at a fixed level (e.g. DEBUG) and writes to the ring. Because TinyUSB
logs arrive as partial fragments (not whole lines), the shim should **not** prepend
a prefix per call (that would inject prefixes mid-line); instead either (a) pass
TinyUSB fragments straight through unprefixed, or (b) do lightweight line-start
detection. Recommend (a) for v1 — point `CFG_TUSB_DEBUG_PRINTF` at a passthrough
that still respects the compiled-out case. (`inc/custom_config.h:63-65`.)

### Ring impact — format-on-produce vs format-on-drain

The ring is bytes today and the producer formats at call time (`src/dlog.c:38-58`).
Two designs:

- **Format-on-produce (recommended for v1).** Keep storing **preformatted text**
  (prefix + message) in the ring. Pros: zero change to the SPSC transport, drain,
  and both sinks; timestamp/core/cat are naturally captured on the producing core;
  no new alignment/atomicity concerns. Cons: the prefix (~28 bytes) consumes ring
  space and the `vsnprintf` cost is paid on core1 — but it already is today, and
  core1's logging is sparse (lifecycle/recovery, not per-SOF). The 16 KiB/core ring
  (`src/dlog.c:15`) easily absorbs the prefix.
- **Structured records (format-on-drain).** Store a fixed binary header
  `{u32 ts, u8 level, u8 cat, u8 core, u16 len}` + raw message, format the prefix in
  `dlog_drain()` on core0. Pros: less core1 CPU and ring bytes per line; enables a
  future binary/host-decoded sink. Cons: variable-length records in a byte ring need
  careful framing to keep the single-store publish atomicity; the message still has
  to be `vsnprintf`'d somewhere (on produce), so most of the cost remains; and
  per-sink color/format logic grows. Defer to a later phase if profiling shows core1
  log cost matters.

Either way the **timestamp and core id are captured on the producer** — that is
non-negotiable for deferred correctness.

### Sinks

- **UART sink** (`uart1`, `src/dlog.c:67-68`): gets ANSI color (default on),
  has its own level threshold. Color applied at drain per-line if we go with the
  "ring holds plain text, sink colorizes" option.
- **USB-stick file sink** (`usb_log_write` → `LOG.TXT`, `src/usb_log.c`): **never**
  colored; its own level threshold (typically equal-or-lower verbosity than UART to
  save flash wear/space). Today it receives whatever the drain emits
  (`src/main.c:591`); add a parameter or second sink-callback signature carrying the
  level so the sink can filter, OR filter per-sink at drain. Simplest: drain passes
  `(data, len, level)` to sinks and each sink decides. (Requires extending the
  `dlog_sink_t` signature — `inc/dlog.h:20` — or adding a small sink-registration
  struct `{fn, min_level, color}`.)

Per-sink level filtering interacts with format-on-produce: if lines are
preformatted text the drain doesn't know each line's level unless we either (a)
keep a parallel level byte per line, or (b) parse the rendered `[LEVEL]` field, or
(c) move to structured records. This is a concrete tension — see open questions.

### Core-safety summary

- No new locks, no `sleep_ms`/`board_millis`/SDK spinlocks introduced; the front
  end uses only `timer_hw->timerawl` and `sio_hw->cpuid`, both lock-free and
  core1-safe ([`FREERTOS-PORT.md`](../FREERTOS-PORT.md) gotcha #2).
- The per-core ring discipline is unchanged: each core writes only its ring head;
  core0 drains both tails (`src/dlog.c`). Runtime-level table is plain `uint8_t`,
  written from core0 config, read from both cores; a torn/stale read only
  admits/drops a single line.
- Full-ring drop behavior is preserved so a chatty core1 can never block the SOF
  (`src/dlog.c:52`).

## Migration plan / phasing

1. **Add the front end, keep everything else.** New `inc/orb_log.h` + `src/orb_log.c`
   implementing levels/cats/timestamp/format on top of the existing `dlog` producer.
   `orb_log_init()` calls `dlog_init()` and sets up sinks. No call-site changes yet.
2. **Compatibility shim.** Redefine `OPENRB_DEBUG(...)` →
   `LOG_INFO(CAT_SYS, ...)` (or a dedicated `CAT_LEGACY`) and `OPENRB_DEBUG_BUF`
   → `LOG_HEXDUMP(CAT_SYS, LOG_LEVEL_DEBUG, …)` in `inc/orb_debug.h`, so all ~40
   existing sites compile and emit through the new pipeline unchanged. Map
   `OPENRB_DEBUG_ENABLED` to the compile-time level.
3. **Point TinyUSB at the shim.** Swap `CFG_TUSB_DEBUG_PRINTF` from `dlog_printf`
   to the `CAT_TUSB` passthrough (`inc/custom_config.h:63-65`).
4. **Migrate call sites by file**, lowest-risk first, assigning real
   levels/categories and dropping hand-written `\r\n` and ad-hoc `[TAG]` prefixes:
   `usb_log.c` (already tagged), `drums.c`/`guitar.c`, `xbox_controller_driver.c`,
   the recovery/announce paths in `main.c`. Each file is an independent, reviewable
   step.
5. **Per-sink level + color config**, then optionally per-category runtime level.
6. **(Optional, later)** evaluate structured-record ring if core1 log cost or a
   binary host decoder becomes desirable.

Backward compatibility holds at every step: `OPENRB_DEBUG` never stops working;
the UART/file sinks and the `housekeeping_task` drain wiring
(`src/main.c:650`, `:541`) are untouched.

## Risks & open questions

- **Format-on-produce vs structured records** — recommended v1 is
  format-on-produce (minimal blast radius), but it complicates per-sink **level**
  filtering at drain (the level isn't separable from the rendered text). Decide:
  accept "UART and file share one level" for v1, OR carry a per-line level byte,
  OR jump straight to structured records. (Leaning: v1 = single shared level +
  global+per-cat runtime gate at *produce* time, so drain-time per-sink level is a
  later nicety.)
- **Runtime vs compile-time level** — is per-category *runtime* control worth the
  table + extra branch, or is a single global runtime level (plus per-category
  compile-time) enough? How is runtime level set with no console today — tie-in to
  [runtime-configurator](runtime-configurator.md)?
- **Color strategy** — colorize-at-sink (ring stays plain text; preferred) vs
  emit-color-and-strip-in-file-sink. Colorize-at-sink needs the drain to know line
  boundaries to wrap SGR codes correctly; the current drain is byte-oriented
  (`src/dlog.c:60-81`) and would need light line-awareness.
- **TinyUSB fragment tagging** — its `CFG_TUSB_DEBUG_PRINTF` output arrives as
  partial line fragments; prefixing per call corrupts lines. v1 passes them through
  untagged-but-bucketed; proper per-line `CAT_TUSB` prefixing needs line-start
  detection in the shim.
- **Timestamp width / wrap** — 32-bit `timerawl` wraps ~71.6 min; acceptable for
  debugging but document it. Do we ever want a 64-bit `timerawh:timerawl` read?
  (Reading both halves on core1 must stay lock-free — the SDK's `time_us_64()` uses
  a spinlock and is therefore **banned on core1** per gotcha #2; a manual hi/lo/hi
  re-read would be needed.)
- **Prefix cost on core1** — adds `vsnprintf` of ~28 prefix bytes per line on the
  PIO-USB core. Core1 logging is sparse today, but a TRACE-level firehose from
  core1 could pressure the ring/SOF; the existing drop-on-full guard
  (`src/dlog.c:52`) bounds the damage. Validate with a TRACE stress test before
  enabling TRACE on core1 by default.
- **Ring sizing** — preformatted lines are longer; confirm 16 KiB/core
  (`src/dlog.c:15`) and the 32 KiB USB ring (`src/usb_log.c:17`) still absorb bursts
  during enumeration/recovery without excessive drops.
- **`dlog_sink_t` signature change** — adding per-sink level/color means either
  changing `dlog_set_sink`/`dlog_sink_t` (`inc/dlog.h:20-21`) or wrapping it; pick
  one to avoid churning `usb_log_write`'s contract (`src/usb_log.c:24`).
- **`stdio`/`vsnprintf` footprint** — already used (`src/dlog.c:42`,
  `inc/orb_debug.h:18`); no new dependency, but float/`%f` in timestamps should be
  avoided (use integer sec/frac split) to keep newlib lean.

## References

- [`docs/FREERTOS-PORT.md`](../FREERTOS-PORT.md) — task model, core1 timing rules
  (gotcha #2: no `sleep_ms`/`board_millis`/spinlocks on core1; `timerawl` is the
  lock-free clock), inter-core channel table (dlog / usb_log rows).
- `src/dlog.c`, `inc/dlog.h` — deferred per-core SPSC rings, producer/drain, sink.
- `inc/orb_debug.h` — current `OPENRB_DEBUG` / `OPENRB_DEBUG_BUF` macros.
- `src/usb_log.c`, `inc/usb_log.h` — USB-stick file sink (FatFs on core1).
- `inc/custom_config.h` — `CFG_TUSB_DEBUG` / `CFG_TUSB_DEBUG_PRINTF` /
  `CFG_TUH_LOG_LEVEL` TinyUSB routing.
- `src/main.c` — `dlog_init`/`dlog_set_sink`/`dlog_drain` wiring
  (`:562`, `:591`, `:650`, `:541`); `housekeeping_task` drain.
- Related specs: [cpp-overhaul](cpp-overhaul.md),
  [runtime-configurator](runtime-configurator.md).
