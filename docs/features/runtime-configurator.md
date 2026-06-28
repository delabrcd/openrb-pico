# Feature: runtime configurator (UI over USB)
Status: PROPOSED (spec only — not yet implemented)
Related: [hihat-mode](hihat-mode.md), [logging](logging.md), [cpp-overhaul](cpp-overhaul.md)

> The related docs above are forward references; some may not exist yet. The hi-hat
> alternate mode in particular is a planned feature whose tunables this configurator is
> expected to expose (see [Config schema](#config-schema--versioning)).

## Summary

Add a host-side GUI that configures the adapter at runtime over USB — the same idea
Santroller ships (a web app talks to the controller over USB, writes config to flash,
reads it + a version back). The user should be able to adjust **timing numbers**
(debounce / hold / output cadence), **MIDI note → drum-output mappings**, and, once it
lands, the **alternate hi-hat mode** toggle/params — without rebuilding and reflashing
firmware. Config persists across power cycles in a dedicated flash sector, loads at boot,
and falls back to compile-time defaults if absent or incompatible.

The hard part is **not** the protocol; it is persisting to QSPI flash on a board whose
timing-critical PIO-USB host (core1) runs straight out of that same XIP flash. This spec
treats the flash write as a deliberate, user-triggered, briefly-stalling "Save" under a
full multicore lockout, and leans on the already-shipping runtime hub-reset recovery to
re-enumerate the controller afterward.

## Motivation

- **Today every tunable is compile-time.** Changing `TRIGGER_HOLD_MS` or remapping a
  MIDI note means editing a header, rebuilding in Docker, and reflashing via SWD
  (`scripts/build.sh` + `scripts/flash.sh`). That is a developer workflow, not a user
  one.
- Drum brains (Roland/Alesis/etc.) emit different MIDI notes per pad/cymbal; the fixed
  table in `inc/default_midi_mapping.tbl` only fits a subset. Users with different kits
  currently cannot self-serve a remap.
- Timing constants (debounce, hold) are kit- and play-style-dependent and are exactly the
  kind of thing users want to nudge and feel the result of, live.
- Santroller has already proven the UX (web configurator over USB) for the adjacent
  RP2040 rhythm-controller audience. Matching that lowers the support burden.

## Goals / Non-goals

**Goals**
- A versioned, magic-guarded config blob covering the timing constants, the MIDI map, and
  the hi-hat-mode params.
- A USB request set: READ config, READ version/identity, WRITE config, COMMIT/persist,
  FACTORY-RESET — reachable from a host app **without disturbing the Xbox device class the
  console depends on**.
- Safe runtime persistence to internal QSPI flash given the XIP/PIO-USB hazard, plus
  boot-time load + validation + fallback.
- A thin description of the host side so the firmware protocol is implementable against a
  real client.

**Non-goals**
- Designing the full web app (only the firmware-facing protocol + a sketch of the client).
- Firmware-update / DFU over this channel (Santroller does this; out of scope here —
  flashing stays SWD/BOOTSEL).
- Per-profile / multi-profile config (Santroller has profiles; we ship one active config).
- Reconfiguring the USB-stick logging or the FreeRTOS task layout at runtime.

## Current state (in code)

Everything below is fixed at compile time.

- **Timing constants** — [`inc/adapter.h`](../../inc/adapter.h):
  `ANNOUNCE_INTERVAL_MS 2000`, `VELOCITY_THRESH 10`, `TRIGGER_HOLD_MS 40`,
  `ON_DELAY_MS 20`, `ADAPTER_OUT_INTERVAL 4`, `ADAPTER_IN_INTERVAL 4`. These are consumed
  directly in [`src/drums.c`](../../src/drums.c) (`note_on` uses `VELOCITY_THRESH`;
  `drum_task` uses `TRIGGER_HOLD_MS` and `ADAPTER_OUT_INTERVAL`) and in
  [`src/main.c`](../../src/main.c) (`announce_task` uses `ANNOUNCE_INTERVAL_MS`).
  `ADAPTER_OUT_INTERVAL`/`ADAPTER_IN_INTERVAL` also feed the endpoint `PollingIntervalMS`
  in the **USB descriptor** ([`src/usb_descriptors.c`](../../src/usb_descriptors.c)) — so
  those two are **not safely runtime-tunable** (they are baked into enumeration; see open
  questions). `ON_DELAY_MS` is currently declared but not referenced in `drums.c`.
- **Recovery constants** — [`src/main.c`](../../src/main.c): `HOST_RECOV_ERR_STREAK 100`,
  `HOST_RECOV_SILENCE_US 30000000`, `HOST_RECOV_GRACE_US 3000000`, `HOST_RECOV_MAX 3`
  (runtime hub-reset path); `RECOV_MAGIC`, `RECOV_MAX 4`, `RECOV_SILENT_MS 3000`
  (watchdog-reboot last resort). Candidate "advanced" tunables (expose read-only or behind
  a guard — see schema).
- **MIDI note → output map** — [`inc/midi_map.h`](../../inc/midi_map.h) includes
  [`inc/default_midi_mapping.tbl`](../../inc/default_midi_mapping.tbl), an X-macro table
  (`MIDI_MAP(note, OUT_*)`). It is expanded **into a `switch` statement** inside
  `get_output_for_note()` in [`src/drums.c`](../../src/drums.c) (lines ~59-69). The
  outputs are the `output_e` enum (`OUT_KICK`, `OUT_PAD_RED`, … `OUT_CYM_GREEN`,
  `NUM_OUT`, `NO_OUT`). **Consequence:** today the map is *code*, not *data*. Making it
  runtime-configurable means replacing the `switch` with a data-driven lookup (an array
  indexed by MIDI note, or a small note→output table walked at runtime). Note several
  notes map to the same output (multiple toms→one pad/cymbal), so the schema must allow
  many-to-one.
- **Hi-hat alternate mode** — not present in code yet (no `hihat` symbols in `src/`/`inc/`).
  Spec it as future config keys so the schema versioning is exercised from day one.
- **USB device class** — the device enumerates as a **vendor-specific** PDP "Rock Band
  Wired Legacy Adapter for Xbox One" (`USB_VID 0x0e6f`, `USB_PID 0x0175`,
  `bDeviceClass = TUSB_CLASS_VENDOR_SPECIFIC`). The config descriptor in
  [`src/usb_descriptors.c`](../../src/usb_descriptors.c) declares **exactly 3 interfaces**
  (`bNumInterfaces = 3`): IF0 (the GIP input EP pair, subclass `0x47` / protocol `0xd0`),
  IF1, IF2 (audio/ancillary, with alt settings). A **custom usbd class driver**
  (`usbd_app_driver_get_cb` → `_xboxd_driver` in
  [`src/xbox_device_driver.c`](../../src/xbox_device_driver.c)) handles the GIP packet
  flow, and `tud_vendor_control_xfer_cb` → `xboxd_control_xfer_cb` already fields
  **vendor control requests on EP0** (it answers the security-method handshake). The
  console's auth/identity flow is sensitive to this descriptor; **changing the interface
  count or layout risks breaking enumeration on the console.** This is the central
  constraint for transport choice.
- **No flash persistence of any app state today.** The only flash the firmware touches at
  runtime is via the watchdog scratch registers (`RECOV_SCRATCH`). All large runtime IO
  goes to a **USB flash drive** through the hub (FatFs), specifically to avoid touching
  XIP — see [logging](logging.md) and
  [`../../../docs/usb-stack-saga.md`](../../../docs/usb-stack-saga.md).

## Design

### Config schema & versioning

One packed C struct, persisted verbatim, guarded by a magic + version + CRC. Keep it
small (target ≤ 512 B so it fits well within one 4 KB flash sector with room to grow).

```c
// inc/config.h  (proposed)
#define ORB_CFG_MAGIC   0x4F524243u   // "ORBC"
#define ORB_CFG_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;        // ORB_CFG_MAGIC
    uint16_t version;      // ORB_CFG_VERSION (bump on incompatible layout change)
    uint16_t size;         // sizeof(orb_config_t) as written (forward-compat, see below)

    // --- timing (mirrors inc/adapter.h) ---
    uint8_t  velocity_thresh;     // VELOCITY_THRESH
    uint8_t  trigger_hold_ms;     // TRIGGER_HOLD_MS
    uint8_t  on_delay_ms;         // ON_DELAY_MS
    uint16_t announce_interval_ms;// ANNOUNCE_INTERVAL_MS
    // NOTE: ADAPTER_OUT/IN_INTERVAL are intentionally NOT here (baked into descriptors)

    // --- MIDI note -> output map ---
    // Dense table indexed by MIDI note 0..127; value is output_e (NO_OUT = unmapped).
    uint8_t  midi_map[128];

    // --- hi-hat alternate mode (forward-looking; ignored until feature lands) ---
    uint8_t  hihat_mode;          // 0 = off
    uint8_t  hihat_param[3];      // reserved tunables (pedal CC, threshold, ...)

    // --- advanced / recovery (read-only in UI v1; reserved) ---
    uint16_t recov_err_streak;    // HOST_RECOV_ERR_STREAK
    uint8_t  reserved[16];        // zero-filled; soaks up minor additions w/o version bump

    uint32_t crc32;               // CRC32 over all preceding bytes (magic..reserved)
} orb_config_t;
```

**Validation on load** (in order): magic matches → `version == ORB_CFG_VERSION` (see
compat) → `size` sane → CRC32 over `[0 .. crc32)` matches. Any failure ⇒ **discard and
use compile-time defaults** (`orb_config_defaults()` initialized from the current
`adapter.h` / `default_midi_mapping.tbl` values), and treat the device as "unconfigured"
(host UI then shows defaults).

**Forward/back-compat strategy:**
- The `size` field + a trailing `reserved[]` pad let us add fields *within the same
  version* as long as old firmware reading a newer-but-same-version blob ignores the tail
  and new firmware reading a shorter blob zero-fills the tail. CRC is computed over the
  written `size`.
- Any change that reinterprets existing bytes (e.g. widening `trigger_hold_ms`, changing
  the `output_e` numbering) is an **incompatible** change ⇒ bump `ORB_CFG_VERSION`. On
  version mismatch v1 simply falls back to defaults (no migration). If migration is ever
  wanted, add a `migrate_vN_to_vN1()` ladder — explicitly out of scope for v1.
- The `output_e` enum values are now part of the on-flash contract; freeze their numbering
  (or store a symbolic table). Recommend freezing.

### USB transport & protocol

**The constraint:** the console talks to interfaces IF0–IF2 as the PDP legacy adapter and
is picky about the descriptor. We must add a config channel that the console ignores and
that a host app can reach.

Three options were evaluated:

| Option | Adds to descriptor? | Console risk | Host reach | Notes |
|---|---|---|---|---|
| **A. Vendor control transfers on EP0** | No new interface (reuses existing `tud_vendor_control_xfer_cb`) | **Lowest** — descriptor unchanged | WebUSB *cannot* issue arbitrary vendor control transfers without a WebUSB-advertised interface; needs a native helper or libusb (WinUSB/MS-OS desc). | Smallest firmware delta; already have the EP0 vendor hook. |
| **B. Separate vendor interface + WebUSB** | Yes — a 4th interface (+ BOS/WebUSB + MS-OS 2.0 descriptors) | **Highest** — extra interface may upset console auth/enum | Best — browser WebUSB talks directly | This is the "configure in the browser" dream but most likely to break the thing the console needs. |
| **C. HID feature-report interface** | Yes — a 4th (HID) interface | Medium — adds an interface but HID is benign/standard | WebHID in browser, or native HID everywhere (no driver) | **This is what Santroller does.** |

**Santroller pattern (studied — reference only).** Santroller's configurator is **HID
feature reports**, *not* WebUSB:
- A dedicated HID config interface
  (`santroller/src/usb/device/hid_config_device.cpp`) whose report descriptor declares
  several **feature reports keyed by report ID** (e.g. `ReportIdConfig 0x22`,
  `ReportIdConfigInfo 0x23`, `ReportIdCommand 0x27`, `ReportIdGetVersion 0x29`,
  `ReportIdGetType 0x32`, plus bootloader/firmware-update IDs out of our scope).
- Host drives it with `GET_REPORT`/`SET_REPORT(FEATURE)`. Read = `GET_REPORT(ConfigInfo)`
  for size+CRC+magic, then stream `GET_REPORT(Config)` in **63-byte chunks**; write = the
  mirror (`SET_REPORT(ConfigInfo)` then chunked `SET_REPORT(Config)`, CRC-checked, then
  commit). Payloads are **protobuf**-encoded.
- **Windows reach via MS OS descriptors** (a vendor control request advertising a GUID),
  **not** a WebUSB BOS descriptor. So the web app uses **WebHID**, and native tools use
  hidapi/pyusb. A 1 Hz **keepalive** report closes the session if the app disconnects.
- Persistence: an EEPROM-emulation lib (`FlashPROM`) over the **last 32 KB of flash**,
  written under `multicore_lockout_start/end_blocking()` (see next section).

**Recommendation: Option A (vendor control transfers on EP0) for v1, with a path to C.**
Rationale: the overriding requirement is *do not disturb the console-facing descriptor*,
and option A is the only one that adds **zero** interfaces — it reuses the EP0 vendor
control path the firmware already implements
(`xboxd_control_xfer_cb`/`tud_vendor_control_xfer_cb`). The cost is host reach: a browser
cannot do arbitrary vendor control transfers, so the v1 host app is a **small native
helper** (Python + pyusb, or a tiny Electron/Tauri app using node-usb/WinUSB). If a
zero-install **browser** experience becomes a hard requirement, add Option C (an HID
feature-report interface) as a *parallel* channel in a later phase and A/B its effect on
console enumeration on real hardware before shipping — adding an interface is exactly the
kind of descriptor change `usb-stack-saga.md` warns is enumeration-sensitive.

**Request set (Option A — vendor control requests on EP0).** Use a single vendor request
type with `bRequest`/`wValue` selecting the operation; chunk via `wIndex` (offset) and the
control `wLength`, mirroring Santroller's chunked transfer but over EP0 instead of HID:

| Op | Dir | bRequest | wValue / wIndex | Payload | Effect |
|---|---|---|---|---|---|
| `CFG_GET_IDENTITY` | IN | 0xC0 | — | `{fw_version, cfg_version, schema_size, board_id, flags}` | version/identity read-back; `flags.configured` says whether a valid blob is in flash |
| `CFG_GET_INFO` | IN | 0xC1 | — | `{magic, version, size, crc32}` | lets host validate before/after |
| `CFG_READ` | IN | 0xC2 | wIndex = byte offset | up to `wLength` bytes of the live RAM-shadow config | chunked read of current (possibly unsaved) config |
| `CFG_WRITE` | OUT | 0xC3 | wIndex = byte offset | config bytes | stream into a staging buffer (RAM); does **not** touch flash |
| `CFG_APPLY` | OUT | 0xC4 | wValue = 0 live / 1 = also persist | — | validate staging (magic/size/CRC) → swap into the live RAM-shadow (live-apply); if wValue=1 also do the flash COMMIT |
| `CFG_COMMIT` | OUT | 0xC5 | — | — | persist current live config to flash (the hazardous op; see below) |
| `CFG_FACTORY_RESET` | OUT | 0xC6 | wValue = 0 RAM only / 1 = erase flash | — | load compile-time defaults into the shadow; optionally erase the flash sector |

All multi-byte fields little-endian. The firmware keeps a **RAM-shadow** `orb_config_t`
(the live config the tasks read) plus a **staging buffer** the host writes into; `CFG_WRITE`
fills staging, `CFG_APPLY` validates+promotes it. This makes a half-finished transfer
harmless and keeps gameplay running on the old config until APPLY.

### Flash persistence (XIP hazard)

**Why this is the dangerous part.** The RP2040 executes code in place (XIP) from external
QSPI flash. Erasing/programming that flash **requires turning XIP off** for the duration —
during which *no core can fetch instructions or read constants from flash*. Core1 runs
**only** the PIO-USB host task and its bit-banged signalling is timing-critical; stalling
it desynchronizes the USB host bus and wedges the CH334R hub / drops the controller. This
is the same class of problem that pushed logging onto a USB stick instead of flash
([`../../../docs/usb-stack-saga.md`](../../../docs/usb-stack-saga.md),
[BUILDING.md](../../BUILDING.md)). So a flash write is **not** something to do casually mid-gameplay.

**Safe write approach (the recommended primary).** Treat COMMIT as a deliberate,
user-initiated, brief "Save" that accepts a momentary USB stall and re-enumerates after:

1. Host triggers `CFG_COMMIT` (after `CFG_APPLY` validated the blob). The console is
   expected to be idle (UI tells the user "do this from the menu, not mid-song").
2. Firmware (on core0, in a dedicated short critical section):
   a. **Park core1 / stop the PIO-USB host.** Under FreeRTOS SMP the
      `multicore_lockout_*` API requires the victim core to have called
      `multicore_lockout_victim_init()`. Two viable ways to make core1 safe to lock out:
      either have `usb_host_task` register as a lockout victim at startup and use
      `multicore_lockout_start_blocking()` (Santroller's mechanism, adapted), **or**
      `vTaskSuspend(usb_host_task)` from core0 and spin until it confirms it is parked off
      the flash. Recommend the explicit lockout-victim path — it guarantees core1 is in a
      RAM-resident spin (not executing XIP) during the erase, which is exactly what the
      hazard demands. (`flash_*` already mark themselves `__not_in_flash_func`; the parked
      core1 handler must also be RAM-resident.)
   b. `save_and_disable_interrupts()` on core0.
   c. `flash_range_erase(CFG_FLASH_OFFSET, FLASH_SECTOR_SIZE)` then
      `flash_range_program(CFG_FLASH_OFFSET, shadow_bytes, padded_len)` (program in
      256-byte `FLASH_PAGE_SIZE` units; one 4 KB sector is ample for ≤512 B config).
   d. `restore_interrupts()`, then `multicore_lockout_end_blocking()` /
      `vTaskResume(usb_host_task)`.
3. **Expect a USB stall + controller drop.** The erase+program is short (single sector,
   low ms) but during it the device stack also can't service the console. After resume,
   **lean on the existing runtime recovery**: the controller may go silent and the
   already-shipping `host_recovery_task` (IN-error-streak / silence ⇒ hub RESET# pulse,
   no reboot — see [`../FREERTOS-PORT.md`](../FREERTOS-PORT.md) and
   [warm-reset-recovery](../warm-reset-recovery.md)) re-enumerates it within ~1–3 s. The
   firmware should return the `CFG_COMMIT` status only after the write completes; the host
   UI shows "Saving… (controller may blink)" and waits for re-enumeration.
4. **Flash location.** Reserve the **last 4 KB sector of flash** for config (`CFG_FLASH_OFFSET
   = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE`), well clear of the program image. Add a
   linker assertion / build check that the firmware image never grows into it. (Santroller
   reserves the last 32 KB; we need far less.)

**Alternatives considered:**
- **Store on the USB stick (FatFs), like the log.** *Pro:* zero XIP hazard — it is the
  whole reason logging went there; no core1 parking, no USB stall, writes are already
  proven safe on this stack. *Con:* requires a stick to be plugged into the hub to save
  *or* load config; config wouldn't survive on a bare adapter; and the stick port may be
  occupied. Good as a **secondary/export** path ("backup/restore config to the stick"),
  not as the primary persistence.
- **RAM-shadow + write-on-save (chosen for the runtime model).** Live config always reads
  from a RAM struct; flash is touched **only** on explicit COMMIT. This is already baked
  into the protocol above (`CFG_WRITE`→staging, `CFG_APPLY`→live, `CFG_COMMIT`→flash) and
  minimizes flash hits (which also matters for flash wear). The tradeoff is that an
  un-committed live config is lost on power-cycle — acceptable and clearly surfaced in the
  UI ("Apply" vs "Save").
- **Write flash without parking core1** — rejected outright; it is the exact failure mode
  the saga documents.

**Read-back (boot load).** On boot, before starting tasks, read the config sector by its
memory-mapped XIP address (`(const orb_config_t *)(XIP_BASE + CFG_FLASH_OFFSET)` — a plain
read, no XIP-off needed), validate (magic/version/size/CRC), and either adopt it into the
RAM-shadow or fall back to `orb_config_defaults()`. Do this in `init()` **before**
`vTaskStartScheduler()` so the first packets already use the right config (mind lesson #1:
no `sleep_ms` pre-scheduler — but a flash *read* needs no delay).

### Boot & apply flow

1. **Boot:** `init()` loads + validates the flash config into the RAM-shadow (or defaults).
2. **Apply at boot:** the tasks read tunables from the shadow instead of the `#define`s:
   - `src/drums.c`: `note_on` reads `cfg.velocity_thresh`; `drum_task` reads
     `cfg.trigger_hold_ms` and `cfg.announce/out_interval`; `get_output_for_note` becomes
     `cfg.midi_map[note]` (data-driven lookup replacing the `switch`).
   - `src/main.c`: `announce_task` reads `cfg.announce_interval_ms`.
   - hi-hat mode (when it exists) reads `cfg.hihat_mode`/`cfg.hihat_param`.
   - The `#define`s in `adapter.h` become the **defaults source** only (used to seed
     `orb_config_defaults()`), not the live values.
3. **Live WRITE during runtime:** `CFG_WRITE`(chunks)→staging, then `CFG_APPLY`(wValue=0)
   validates and atomically swaps the RAM-shadow → tunables take effect on the next task
   tick (no flash, no stall — great for the "tweak and feel it" loop). Concurrency: the
   shadow is read by `drum_task` (core0) and `announce_task` (core0); APPLY runs in the
   EP0 control handler (core0) — same core, so a swap of a pointer-to-shadow (or a brief
   critical section around the struct copy) is sufficient. The MIDI-map is read on core0
   only. No cross-core sharing of the config is required for v1 (PIO-USB host on core1
   doesn't read app tunables).
4. **Persist:** `CFG_APPLY`(wValue=1) or a later `CFG_COMMIT` performs the hazardous flash
   write described above. Recommend a separate explicit COMMIT so the user can experiment
   live and only Save when happy.
5. **Identity read-back:** `CFG_GET_IDENTITY` returns firmware version + schema version +
   "configured?" flag at any time so the host app can detect mismatches and offer an
   upgrade/reset.

### Host app (brief)

Scope here is the firmware protocol; the app is sketched only.

- **v1 (Option A): a thin native helper.** Python + pyusb (cross-platform; on Windows the
  device needs a WinUSB binding, advertised via an **MS OS 2.0 descriptor** added to the
  device — a control-transfer-only addition, no new interface) or a small Tauri/Electron
  app over node-usb. It implements: connect → `CFG_GET_IDENTITY` → `CFG_READ` (render
  form) → user edits → `CFG_WRITE`+`CFG_APPLY` (live preview) → `CFG_COMMIT` (Save) →
  optional `CFG_FACTORY_RESET`. UI exposes timing sliders, a MIDI-learn note-mapper (press
  a pad, capture the incoming note, bind to an output), and the hi-hat toggle.
- **If browser-zero-install becomes required:** mirror **Santroller's WebHID** approach by
  adding the HID feature-report interface (Option C) and reusing the same op set as HID
  report IDs. Defer until A is proven and the descriptor change is A/B-tested on console.
- The app should treat `CFG_GET_IDENTITY.flags.configured == false` as "device on
  defaults" and a version mismatch as "this app is older/newer than the firmware —
  re-flash or update."

## Implementation plan / phasing

1. **Schema + defaults + boot load (no USB yet).** Add `inc/config.h`/`src/config.c`:
   `orb_config_t`, `orb_config_defaults()` seeded from `adapter.h` +
   `default_midi_mapping.tbl`, CRC32, and the boot-time XIP read+validate into a RAM-shadow.
   Make the tasks read from the shadow (replace the `#define`s and the
   `get_output_for_note` `switch` with `cfg.midi_map[]`). Ships value (data-driven map)
   even before any UI.
2. **Flash COMMIT under multicore lockout.** Implement the core1-park + erase+program of
   the last sector; validate the re-enumeration behavior on **physical hardware** (per
   saga: SWD resets are not representative). Gate behind a debug command first.
3. **EP0 vendor protocol.** Wire the request set into `xboxd_control_xfer_cb`
   (`CFG_GET_IDENTITY/INFO/READ/WRITE/APPLY/COMMIT/FACTORY_RESET`), staging buffer,
   live-apply swap. Add the MS OS 2.0 descriptor for Windows reach.
4. **Native host helper** (Python/pyusb reference client) implementing read/edit/apply/
   save + MIDI-learn.
5. **(Optional, later) hi-hat config keys** wired once that feature lands; exercises a
   no-version-bump field addition via `reserved[]`.
6. **(Optional, later) HID/WebHID interface** if a browser app is required — A/B console
   enumeration before/after on hardware.

## Risks & open questions

- **Transport choice (biggest):** Option A (EP0 vendor control) keeps the descriptor
  untouched but rules out a pure-browser app; Option C (HID/WebHID) enables the browser at
  the cost of a descriptor change the console may reject. **Recommendation: A for v1, C
  only after on-hardware A/B.** Open: does the Xbox One auth flow tolerate added vendor
  control `bRequest`s on EP0? (Likely yes — it already shares EP0 with the security
  handshake — but verify the request codes don't collide with the GIP/auth ones.)
- **Flash-vs-stick persistence:** recommend internal-flash COMMIT (works on a bare
  adapter) with USB-stick export as backup. Open: how reliably does the controller
  re-enumerate after an in-field flash write across many *physical* power states? Must be
  measured on hardware, not SWD (saga §"Critical testing methodology").
- **core1 parking mechanism under FreeRTOS SMP:** `multicore_lockout_victim_init()` on the
  host task vs `vTaskSuspend`. The lockout API is the documented-safe one but must be
  reconciled with the FreeRTOS SMP scheduler owning core1 (Santroller is bare-metal on
  core1). Needs a spike. Mind FREERTOS-PORT lessons #1/#2 (no SDK spinlocks/`sleep_ms` on
  core1).
- **Schema versioning:** freeze the `output_e` numbering as on-flash contract; decide now
  whether v1 does any migration (recommend: no — fall back to defaults on mismatch).
- **`ADAPTER_OUT/IN_INTERVAL` are in the descriptor**, so they can't change at runtime
  without re-enumeration. Decide: expose them read-only, or omit from config (current
  spec omits them). Same question for the recovery constants (recommend read-only/reserved
  in v1).
- **Mid-gameplay COMMIT abuse:** a user could hit Save mid-song and drop the controller.
  Mitigate in UI copy + possibly refuse COMMIT while `adapter_get_state() == STATE_RUNNING`
  with active input. Open question whether to hard-block.
- **Flash wear / power-loss during erase:** single-sector, rare (only on Save) ⇒ wear is
  a non-issue. Power loss mid-erase leaves an invalid sector ⇒ CRC fails ⇒ boots to
  defaults. Acceptable; document it.

## References

- Code (this repo):
  [`inc/adapter.h`](../../inc/adapter.h),
  [`inc/midi_map.h`](../../inc/midi_map.h),
  [`inc/default_midi_mapping.tbl`](../../inc/default_midi_mapping.tbl),
  [`src/drums.c`](../../src/drums.c) (`get_output_for_note`, `note_on`, `drum_task`),
  [`src/main.c`](../../src/main.c) (`announce_task`, recovery constants/tasks, `reset_usb_hub`),
  [`src/usb_descriptors.c`](../../src/usb_descriptors.c) (3-interface vendor descriptor),
  [`src/xbox_device_driver.c`](../../src/xbox_device_driver.c)
  (`xboxd_control_xfer_cb` / `tud_vendor_control_xfer_cb`, `usbd_app_driver_get_cb`),
  [`inc/custom_config.h`](../../inc/custom_config.h).
- Docs: [`../FREERTOS-PORT.md`](../FREERTOS-PORT.md) (task model, core1 = PIO-USB only,
  runtime recovery), [`../warm-reset-recovery.md`](../warm-reset-recovery.md),
  [`../../BUILDING.md`](../../BUILDING.md) (flash/log-to-stick),
  [`../../../docs/usb-stack-saga.md`](../../../docs/usb-stack-saga.md) (XIP/PIO-USB hazard,
  hardware-testing caveats).
- Santroller (third-party reference, `../../../santroller`): config-over-USB =
  **HID feature reports + protobuf**, MS-OS-descriptor for Windows (not WebUSB), EEPROM
  emulation in the last 32 KB of flash written under `multicore_lockout_*`. Key files:
  `src/usb/device/hid_config_device.cpp`, `lib/FlashPROM/src/FlashPROM.cpp`,
  `proto/config.proto`, `src/config.cpp`.
