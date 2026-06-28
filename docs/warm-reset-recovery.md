# Controller recovery (warm-reset & runtime wedge)

How the adapter brings the console controller back online when it goes dead — either
after a **warm reset** of the RP2040 (an SWD/`SYSRESETREQ` reset, a physical
RESET-button press, or a firmware/watchdog reboot) or when the running hub/controller
**wedges mid-session** — without the user having to replug it.

This is one of the genuinely hard problems on the rev-0.2 board. The short version:
the controller cannot be power-cycled by firmware, so it can get stuck in a broken USB
state (a "silent zombie" — still mounted but sending nothing). The root cause is a
hardware limitation; the real fix is a hardware change (see [The real fix](#the-real-fix)).
Until then we recover in software, and the recovery story has **two tiers**:

1. **Runtime hub-reset recovery (preferred, no reboot)** — the running firmware detects
   the wedge and pulses the hub's RESET# to force a clean re-enumeration *in place*. See
   [Primary recovery](#primary-recovery--runtime-hub-reset-no-reboot). This is the path
   that handles a mid-session wedge and, since the FreeRTOS SMP port, the warm-reset
   zombie too.
2. **Watchdog-reboot recovery (last resort)** — the older, *probabilistic*
   reboot-and-retry, kept as a fallback for when the runtime path can't bring the
   controller back. See [Last resort](#last-resort--heartbeat-keyed-watchdog-reboot).

The runtime path lives on core1 in the FreeRTOS SMP build — see
[`FREERTOS-PORT.md`](FREERTOS-PORT.md) ("Runtime controller recovery") for how it fits
the task topology.

---

## Symptom

- **Cold boot / power-on:** controller enumerates and works (LED on, input flowing).
- **Replug the controller:** works.
- **Warm reset of the RP2040 (no power cycle):** the controller often does **not**
  come back — its LED stays off and it sends no input — even though the firmware is
  running fine and the USB hub is healthy.

The adapter only needs the controller to **announce to the console and authenticate**;
once authenticated, drum input arrives over serial and the controller is optional. But
auth requires a *working* controller at boot, so an unreliable warm-reset bring-up
breaks the console use case (the console resets the adapter during negotiation).

## Root cause

On rev 0.2 the **downstream hub VBUS is hardwired +5 V** — the firmware cannot cut
power to the controller (there is no `PIN_5V_EN` for the downstream ports). So across
an RP2040 warm reset:

- The CH334R hub stays powered (and re-enumerates cleanly — the hub is never the
  problem).
- The **controller stays powered and keeps its old USB session state** (its old
  address, its "configured" state). Nothing ever told it the host went away — the
  PIO-USB host just vanished mid-conversation.

When the firmware comes back and the host re-enumerates, two distinct failures show up,
in order:

### 1. Enumeration fails — the controller ignores the port reset

USB-trace (`CFG_TUSB_DEBUG=2`) of a failing warm boot:

```
HUB init -> HUB opened -> PORT_POWER ports 1-4 -> HUB mounted (addr 5)   # hub is fine
HUB sees device on port 2 -> Resetting Device -> PORT_RESET port 2 -> PORT_RESET_CHANGE
Get Device Descriptor -> [1:0] Control FAILED, 0 bytes                    # no response
Enumeration attempt 2/3 -> FAILED
Enumeration attempt 3/3 -> FAILED
```

The hub issues a real downstream `PORT_RESET`, but the controller does **not** respond
to the address-0 `GET_DESCRIPTOR` afterward (zero bytes, repeatedly). It's still bound
to its old address / old session — the single port reset the CH334R drives is too
marginal to knock it back to address 0. The standard TinyUSB retry only re-sends the
(doomed) control transfer to an address the controller isn't listening on; it never
re-resets the port.

### 2. The "zombie" — enumerated but silent

Even once we *do* get it to enumerate (see [the enum re-reset
fix](#supporting-fix--re-reset-the-hub-port-on-every-enum-attempt) below), a warm-reset
controller can
come back as a **zombie**: it mounts (`Controller 0 Connected`) but never sends a
single packet — no heartbeat (`CMD_STATUS`), no input (`CMD_INPUT`), LED off. Compare:

| boot type | mounts? | sends heartbeat/input? |
|-----------|---------|------------------------|
| power-on (POR) | yes | **yes** (works) |
| warm reset (sometimes) | yes | **no** (zombie) |

This is the trap: `Controller Connected` (USB mount) is **not** proof the controller
works. The controller's USB peripheral needs an actual power cycle to fully
re-initialise; a bus reset gets it far enough to enumerate but not to run. The only
true liveness signal is **its packet stream**.

Whether a given warm reset yields a live controller or a zombie is **probabilistic** —
which is why pressing physical RESET a second time often fixes it.

## The fix

The recovery is layered. The **primary** mechanism (added with the FreeRTOS SMP port)
is a runtime, non-reboot hub reset; a TinyUSB-fork tweak makes the re-enumeration
*succeed*; and the old auto-reboot is now only a **last resort**.

### Primary recovery — runtime hub-reset (no reboot)

`host_recovery_task()` in `src/main.c` runs on **core1**, inside `usb_host_task` (the
sole core1 task — see [`FREERTOS-PORT.md`](FREERTOS-PORT.md)). When the controller goes
silent-zombie it pulses the hub's **RESET#** to force a clean re-enumeration **without
rebooting the RP2040** — so it works **pre- and post-auth**, unlike the reboot path.

Asserting RESET# drops the hub's upstream pull-up → the root port finally sees SE0 →
Pico-PIO-USB raises a disconnect → TinyUSB's `process_removed_device()` tears down the
hub and, recursively, the downstream controller (a clean umount), then re-enumerates the
whole tree on release. No `tuh_deinit` / host re-init — it leans on the normal,
well-tested attach/detach path; only the wedged silicon needs the RESET# kick. (Custom
board only — `FEATHER` has no hub, so the task compiles out.)

It detects the wedge **two ways** (constants in `src/main.c`):

- **Fast (primary):** a run of consecutive interrupt-IN failures —
  `xboxh_in_error_streak() >= HOST_RECOV_ERR_STREAK` (100, ~1.25 s). A wedged CH334R
  fails the controller's IN poll continuously (~80/s), whereas a healthy *idle* pad
  produces **no** IN completions between its sparse ~20 s `CMD_STATUS` heartbeats (NAKs
  don't complete) — so the streak is **false-positive-free** and recovers ~1–3 s after a
  wedge. The streak signal lives in `src/xbox_controller_driver.c`
  (`xboxh_in_error_streak()` / `xboxh_clear_error_streak()`).
- **Backstop:** total silence longer than `HOST_RECOV_SILENCE_US` (30 s), in case a
  wedge ever stops the IN poll entirely instead of failing it. The threshold must exceed
  the pad's ~20 s idle keep-alive (which refreshes `g_host_last_rx_us`), so 30 s clears
  the heartbeat with margin. In active play input streams sub-second, so neither path
  fires spuriously.

It is **bounded and self-clearing**: at most `HOST_RECOV_MAX` (3) RESET# pulses, spaced
`HOST_RECOV_GRACE_US` (3 s) apart so each re-enumeration can land before the next. It
only arms if a controller actually mounted this boot (`adapter_controller_seen()`), and a
single fresh heartbeat (`g_host_rx_count` advances) exits recovery and refills the
budget. If the budget is exhausted it **stands down** (a clean unplug also looks like a
wedge, so it won't thrash the hub forever) and hands off to the last-resort reboot path.

That hand-off is the `g_runtime_recovery_engaged` flag: while the runtime path is
actively recovering, it is `true` and the core0 watchdog-reboot path **defers**; it goes
`false` only once the runtime path gives up, letting the reboot path take its turn.

### Supporting fix — re-reset the hub port on every enum attempt

In the vendored TinyUSB fork (`external/tinyusb`, branch `openrb`),
`host/usbh.c` `process_enumeration()`:

> On each enumeration retry, while still at address 0 behind a hub, re-issue
> `hub_port_reset()` instead of blindly re-sending the control transfer. The CH334R's
> single reset is marginal; hitting the port again on each attempt knocks the frozen
> controller back to address 0 so it enumerates. `ATTEMPT_COUNT_MAX` bumped 3 -> 6.

This makes the controller *enumerate* reliably. (The same commit also reverts an
earlier `PORT_POWER` connection-scan experiment, which destabilised the hub and broke
replug — a dead end; the re-reset is the real fix.)

### Last resort — heartbeat-keyed, auth-gated, bounded auto-reboot

This is the **original** recovery and is now the **fallback**: it only acts when the
runtime hub-reset path above has stood down (`g_runtime_recovery_engaged` is `false`).
It full-reboots the chip and retries, which the runtime path avoids — so prefer the
runtime path; this exists for the cases it can't fix.

In `src/main.c` (`recovery_reboot_task`), because even a successful re-enumeration can
still yield a zombie:

- **Liveness = the packet stream, not mount.** `g_controller_alive` is set in
  `xboxh_packet_received_cb` (the first real packet from the controller), and reset on
  umount. Mount alone (`xbox_controller_idx`) is *not* trusted.
- **Retry by rebooting.** Recovery is probabilistic, so if a controller mounted but is
  silent past `RECOV_SILENT_MS` (3 s), `watchdog_reboot()` and try again. A later boot
  almost always lands a live controller (validated: zombie -> reboot -> heartbeat
  flowing). This automates the "press RESET again" that a human would do.
- **Bounded — never loops forever.** Up to `RECOV_MAX` (4) reboots. The attempt count
  rides through our own watchdog reboots in **watchdog `scratch[7]`** (gated on
  `watchdog_caused_reboot()`), and is cleared on a fresh power-on / physical reset, so
  the user always gets a clean budget. `scratch[4..6]` are off-limits — the SDK/bootrom
  use them for the watchdog reboot magic and `watchdog_caused_reboot()`.
- **Gated on authentication.** A controller is needed to *establish* auth (and to
  re-establish it after a console-initiated reset), so recovery is armed before auth and
  retries a zombie or a controller lost part-way through auth. Once `adapter_state`
  reaches `STATE_RUNNING` (authenticated), the controller is optional — the user may
  unplug it freely — so recovery **disarms permanently** for that session and a
  post-auth unplug never triggers a reboot.
- **No controller, no reboot.** If nothing ever mounts (`g_controller_seen` false),
  there's no controller to recover, so we don't reboot — we come up controllerless
  instead of reboot-dancing.

### End-to-end behaviour

| situation | behaviour |
|-----------|-----------|
| controller comes back live | works first try, no recovery action |
| controller wedges mid-session (any time) | runtime hub-reset (≤3) brings it back, **no reboot** |
| warm reset, controller is a zombie | runtime hub-reset (≤3) first; only if that gives up does the watchdog reboot (≤4) take over |
| controller lost before/during auth | runtime hub-reset; reboot as last resort |
| controller unplugged **after** auth | runtime path stands down; no reboot; adapter keeps running (drums over serial) |
| no controller plugged at all | comes up without one, no recovery loop |
| zombie that never thaws (hub-resets *and* reboots exhausted) | gives up, waits for a replug |

## A related fix: post-auth controller replug (re-init on re-announce)

Different trigger, same family. *After* auth (`STATE_RUNNING`), recovery is disarmed —
the controller is optional, so a warm-reset reboot would be wrong. But the user can
still **replug** the controller (e.g. to swap it). On a replug that fully re-powers the
controller's GIP layer, it comes up sending `CMD_ANNOUNCE` repeatedly and **never
streams input** — because our only controller init is the one-shot power-on/LED in
`xboxh_set_config()`, fired at mount, which the just-attached controller announces
*after* and so misses. Nothing in the old code responded to a `CMD_ANNOUNCE`, so it
announced forever.

Contrast with a *quick* replug where the controller keeps its powered/configured state:
it just resumes `CMD_INPUT`/`CMD_STATUS` with no announce. The failure is specifically
the controller that fully re-initialised its GIP state and is waiting for the host.

**Fix:** respond to the re-announce. `handle_controller_packet_running()` flags a
`CMD_ANNOUNCE` (running state only); the **core1** host loop services it by calling
`xboxh_reinit_controller()` (clears `is_powered`, re-sends the power-on/LED). Debounced
to ~2×/s and run from the loop rather than the host callback, because the init blocks on
tx (`wait_for_tx_complete` pumps `tuh_task`) and must not re-enter a callback. The
controller streams again within one re-init.

This also cleared a secondary symptom: a `CMD_DROP_PLAYER` (drums unplugged) that the
console ignored. The drop's device-side transfer never completed (`sending
CMD_DROP_PLAYER` with no matching `OUT (CMD_DROP_PLAYER)`) — the announce flood was
starving core0's device servicing. With the controller re-initialised the flood stops
and the drop transfers and registers normally. The drop path itself was unchanged.

- **Console-validated:** authed, replugged the controller into the bad announce-only
  state, observed `Controller re-announced -> re-init` followed by `CMD_INPUT`/
  `CMD_STATUS` resuming (52 input / 31 status, zero further announces), and `OUT
  (CMD_DROP_PLAYER)` completing with the player dropping on-screen.

## Validation status

- **Runtime hub-reset validated on hardware** (FreeRTOS SMP port): a controller dropped
  mid-session — including the hub wedge a debug halt induces by freezing SOF — comes back
  ~1–3 s later via the RESET# pulse, with a `HOST RECOVERY: controller wedged -> hub
  reset` line and a clean reconnect on the UART, **no reboot**. See
  [`FREERTOS-PORT.md`](FREERTOS-PORT.md).
- **Bench-validated** (no console attached): zombie recovery reliably lands a *live*
  controller — observed `zombie -> watchdog reboot -> heartbeat (CMD_STATUS) flowing`,
  and clean multi-reset batches come up live. The bench keeps recovery permanently
  armed (auth is never reached without a console), which exercises the recovery path.
- **Console-validated:** the auth *disarm* path triggers at `STATE_RUNNING` (real auth
  handshake), and the post-auth replug re-init (above) was confirmed on a live console
  session — a song played through with serial-MIDI drums, controller replugs recovered,
  and drop-player registered.

## The real fix

Reliable, deterministic warm-reset recovery requires **power-cycling the controller**,
which rev-0.2 hardware cannot do. The hardware fix is a **firmware-controlled
downstream VBUS FET** (the `PIN_5V_EN` the rev-0.2 board lacks for downstream ports) in
a board respin. Then a warm reset can drop and restore the controller's power and it
re-initialises cleanly — no hub-reset pulse, no re-reset, no zombie, no reboot dance.
Until then, the software workaround above (runtime hub-reset, with reboot as last resort)
is the ceiling.

## Gotchas learned along the way

- **Rapid SWD reset *batches* wedge the hub.** The CH334R accumulates bad state across
  back-to-back resets and eventually nothing enumerates until a full power cycle. This
  confounds reliability measurement over SWD. **Watchdog reboots and physical RESET
  presses are clean** (they behaved like real recovery, not like the SWD batches) — use
  those, in small batches with a power cycle between them, for ground truth.
- **`Unknown flash device (ID 0x00ffffff)`** when flashing = the QSPI is wedged in
  continuous-read mode (can follow over-aggressive SWD resetting); needs a physical
  power cycle to recover.
- **`grep` the UART log with `-a`.** Stray binary bytes from the UART make `grep` switch
  to binary mode and silently emit nothing (even `grep -c` prints no count). `grep -a`
  forces text mode and avoids hours of "why is this empty" confusion.
