# Feature: alternate hi-hat mode (CC-keyed)
Status: IMPLEMENTED (gated) — behind compile-time `ORB_HIHAT_MODE` in
`inc/hihat_config.h` (0=off/default, byte-identical build; 1=CC parsing + discovery log;
2=CC-keyed override). CC parsing added to the serial path; state machine + override in
`drums.c`. Phase 2 (runtime-config-backed, persisted tunables) pends the runtime
configurator; needs bench tuning of the kit's actual CC#/polarity.
Related: [runtime-configurator](runtime-configurator.md) (companion spec — also PROPOSED; provides the persisted-settings mechanism this feature stores its tunables in)

## Summary
Add a second way to decide whether a hi-hat strike is reported to the console as
**open** or **closed**. Today the firmware is a pure note relay: it trusts the drum
kit to send a different MIDI note for open vs. closed hi-hat, and maps each note to a
fixed Rock Band output. The new **CC-keyed** mode instead tracks the hi-hat *pedal
position* (a MIDI Control Change, continuous 0..127) and, on any hi-hat strike note,
emits open or closed based on the tracked pedal state — ignoring whichever open/closed
note the kit happened to send. Mode and tunables are runtime-configurable and default
to the current relay behavior.

## Motivation
The owner's words: *"Right now we depend on the drum kit reporting the correct thing
for hi-hat controls. I'd like an alternate mode that keys off the MIDI control signals
for whether the hi-hats are open/closed and uses that instead of just relaying the
notes."*

Concretely, correctness today depends entirely on the kit's internal mapping:

- Some kits emit a different note number for the open vs. closed hi-hat zone, but the
  note chosen depends on **which zone was struck**, not on the **pedal position** — so
  a player who half-presses or chokes the pedal gets the "wrong" open/closed result.
- Cheaper kits emit one fixed hi-hat note regardless of pedal, so open/closed is simply
  unavailable in relay mode.
- The pedal position is information the kit *does* send (as a continuous CC), but the
  firmware currently discards it (see [Current state](#current-state-relay-mode-in-code)).

CC-keyed mode makes the adapter the authority on open/closed, driven by the actual
pedal, independent of the kit's note choices.

## Goals / Non-goals
**Goals**
- A mode toggle: `RELAY` (default, current behavior — bit-for-bit unchanged) vs.
  `CC_KEYED`.
- In CC-keyed mode, track hi-hat openness from a configurable pedal CC with a threshold
  and hysteresis, and override the open/closed output for hi-hat strike notes.
- Make the watched CC number, threshold, hysteresis, polarity, and the hi-hat note set
  configurable and persisted via the runtime configurator.
- Add ControlChange parsing/routing to the MIDI path (currently NoteOn-only), without
  disturbing PIO-USB timing or the existing relay path.

**Non-goals**
- Changing the default behavior when the feature is off.
- Velocity-proportional / "how open" reporting — RB pro-drums is binary per lane; we
  only decide open vs. closed.
- Inferring pedal position from notes, or auto-detecting the kit. The CC and note set
  are configured, not learned.
- Touching the cross-core USB/timing architecture (see
  [FREERTOS-PORT.md](../FREERTOS-PORT.md)).

## Background: hi-hat in MIDI & in Rock Band pro-drums
**On the kit (MIDI).** A hi-hat produces two kinds of message:
- **Strike** — a NoteOn when a zone is hit. General-MIDI percussion uses note **42**
  (Closed Hi-Hat), **46** (Open Hi-Hat), and **44** (Pedal Hi-Hat); Roland/edge zones
  commonly add **22** (closed edge) and **26** (open edge). The default map below uses
  exactly {22, 26, 42, 46}.
- **Pedal position** — a continuous Control Change as the foot pedal moves. **CC#4
  ("Foot Controller")** is the most common choice (used by many Roland TD and Yamaha
  DTX modules for hi-hat pedal position), but it is kit-specific. Value range is 0..127.
  Polarity is also kit-specific: on many modules a *low* value means open (pedal up) and
  a *high* value means closed (pedal down), but this is not universal — hence the
  configurable invert flag below.

**In Rock Band pro-drums (the output side).** The adapter reports per-lane booleans —
there is no continuous "openness." The relevant outputs are the colored pads and
cymbals (`output_e` in [`src/drums.c:16`](../../src/drums.c)): `OUT_CYM_YELLOW`,
`OUT_CYM_BLUE`, etc., which set the bitfields `cymbal_yellow` / `cymbal_blue` in
`xb_one_drum_input_pkt_t` ([`inc/xbox_one_protocol.h:154-160`](../../inc/xbox_one_protocol.h)).

The firmware's existing convention for representing open vs. closed is encoded directly
in [`inc/default_midi_mapping.tbl`](../../inc/default_midi_mapping.tbl):

```
MIDI_MAP(22, OUT_CYM_YELLOW)   // closed hi-hat (edge)   -> yellow cymbal
MIDI_MAP(42, OUT_CYM_YELLOW)   // closed hi-hat (GM 42)  -> yellow cymbal
MIDI_MAP(26, OUT_CYM_BLUE)     // open hi-hat (edge)     -> blue cymbal
MIDI_MAP(46, OUT_CYM_BLUE)     // open hi-hat (GM 46)    -> blue cymbal
```

So the established mapping is **closed hi-hat → yellow cymbal**, **open hi-hat → blue
cymbal**. CC-keyed mode must reproduce *this same output convention*, just driven by the
pedal CC instead of the note number. (Whether yellow/blue is the ideal RB representation
of open/closed is an [open question](#risks--open-questions); this spec adopts the
existing default-map convention so CC-keyed and relay modes agree when the kit behaves.)

> Note: blue cymbal is **not** hi-hat-exclusive — notes 51/53/59 also map to
> `OUT_CYM_BLUE` (ride/crash) in the default map. Therefore the set of "hi-hat strike"
> notes cannot be inferred from the output color and must be declared explicitly (see
> the configurable hi-hat note set below).

## Current state (relay mode, in code)
The data path (per [FREERTOS-PORT.md](../FREERTOS-PORT.md)):

1. **USB-MIDI (core1):** `drums_read_midi_host()` drains the host MIDI FIFO and pushes
   each message's first 3 bytes onto the `midi_note` queue
   ([`src/drums.c:138-145`](../../src/drums.c)). It copies the bytes **raw, for any
   message type** — it does not filter on status. *CC messages already reach the queue.*
2. **Serial MIDI (core0):** `serial_midi_read()`
   ([`src/midi.c:75-119`](../../src/midi.c)) is a hand-rolled parser that treats
   **only `NoteOn` as a valid status byte** (`case NoteOn:` sets `status_byte`/`count`;
   everything else falls to `default:` which resets `count = 0`). A ControlChange status
   (`0xB0`) lands in `default` and its data bytes are dropped. *CC is lost on the serial
   path.*
3. **Consumer (core0):** `drum_task()` drains both sources and acts on NoteOn **only**:
   ```c
   if (type == NoteOn) note_on(n.data[1], n.data[2]);          // queue (USB)  drums.c:157
   if (type == NoteOn) note_on(pending_msg[1], pending_msg[2]); // serial       drums.c:162
   ```
   CC messages that *do* arrive via the queue are read and discarded (no branch handles
   them).
4. **Mapping + output:** `note_on()` → `get_output_for_note(note)`
   ([`src/drums.c:59-69`](../../src/drums.c), driven by the `MIDI_MAP` table) →
   `update_drum_state_with_midi_input()` sets the lane bit. Hits auto-clear after
   `TRIGGER_HOLD_MS` (40 ms) and packets emit at most every `ADAPTER_OUT_INTERVAL`
   (4 ms); velocity ≤ `VELOCITY_THRESH` (10) is ignored
   ([`inc/adapter.h:14-21`](../../inc/adapter.h)).

**Limitation, precisely:** open vs. closed is decided 100% by which note the kit sends
(`get_output_for_note` is a static note→lane lookup with no pedal awareness). The pedal
CC is either dropped (serial) or read-and-discarded (USB). The adapter cannot correct or
override the kit's open/closed decision.

## Design (CC-keyed mode)
All new state and logic live in `drums.c` on **core0** (where `drum_task` already
consumes both MIDI sources), so no new cross-core sharing is introduced. CC and notes
arrive interleaved on the same FIFO-ordered streams and are processed in arrival order
within each `drum_task` tick.

### Pedal-openness state machine
Maintain one boolean in `drums.c`:

```c
static bool g_hh_open = false;   // false = closed (pedal down) ; init closed
```

On each ControlChange whose controller number equals the configured `hihat_cc`, update
with threshold + hysteresis (polarity per `hihat_invert`):

```c
// "raw closeness" rises with the configured-closed direction
uint8_t v = hihat_invert ? (127 - value) : value;   // v high = more closed
if (g_hh_open) {
    if (v >= hihat_threshold + hihat_hyst) g_hh_open = false; // -> closed
} else {
    if (v <= hihat_threshold - hihat_hyst) g_hh_open = true;  // -> open
}
```

Defaults: `hihat_threshold = 64`, `hihat_hyst = 8`, `hihat_invert = false`. Hysteresis
prevents chatter when the pedal hovers near the threshold. Initial state is **closed**
(matches a resting/unknown pedal mapping to the default yellow-cymbal lane).

### Output override on hi-hat strikes
Define an explicit, configurable **hi-hat strike note set** (default `{22, 26, 42, 46}`).
A new predicate `is_hihat_note(note)` tests membership. In `note_on()`, after the
existing `out = get_output_for_note(note)`:

```c
if (hihat_mode == CC_KEYED && is_hihat_note(note)) {
    out = g_hh_open ? OUT_CYM_BLUE : OUT_CYM_YELLOW;   // override the table's choice
}
```

Everything downstream (dedup via `midi_output_states[out].triggered`, hold/aging, packet
build) is unchanged. In `RELAY` mode the branch is skipped and behavior is byte-identical
to today.

Rationale for overriding in `note_on()` rather than in `get_output_for_note()`:
`get_output_for_note` is a pure `switch` over the compile-time map and is
`__not_in_flash_func`; keeping it pure (and the pedal-dependent decision in `note_on`)
avoids threading runtime state through the table lookup and keeps the override one place.

### Pedal note (note 44)
GM note 44 (Pedal Hi-Hat / pedal chick) is unmapped today. This spec leaves it unmapped
by default; it is *not* in the hi-hat strike set (it is a pedal sound, not a stick
strike). Treating a pedal chick as a closed-hat hit is a possible future tunable, noted
as an open question.

### Config (persisted via the runtime configurator)
New settings, defaults chosen so the feature is off and behavior is unchanged:

| key | type | default | meaning |
|---|---|---|---|
| `hihat_mode` | enum | `RELAY` | `RELAY` or `CC_KEYED` |
| `hihat_cc` | u8 (0..127) | `4` | pedal-position controller number to watch |
| `hihat_threshold` | u8 (0..127) | `64` | open/closed split point |
| `hihat_hyst` | u8 | `8` | hysteresis band around the threshold |
| `hihat_invert` | bool | `false` | flip CC polarity (high value = open) |
| `hihat_notes` | u8[] | `{22,26,42,46}` | strike notes treated as hi-hat (optional/Phase 2) |

These are read by `drums.c` at runtime. See [runtime-configurator](runtime-configurator.md)
for the storage/transport mechanism (this spec assumes that doc provides typed,
persisted, runtime-mutable settings; if it lands later, Phase 1 can ship with
compile-time `#define`s as a stopgap — see phasing).

## Required changes
1. **`src/midi.c` — parse ControlChange on the serial path.** Extend
   `serial_midi_read()` so `ControlChange` (`0xB0`) is accepted as a status byte
   (like `NoteOn`): set the status, collect its 2 data bytes, and return the raw 3-byte
   message in `buf`. The return contract stays "3 raw bytes; caller inspects the status";
   `get_type_from_status` already strips the channel nibble. Active-sense/connect
   bookkeeping is unaffected (a CC status byte should also count as "drums connected",
   same as a note). Running-status is out of scope (the kit re-sends status per message
   today).
2. **`src/drums.c` — route CC to the openness state machine.** In `drum_task()`, add a
   `ControlChange` branch alongside the two `NoteOn` branches, for both the `midi_note`
   queue and the serial path, calling a new `control_change(controller, value)` that
   runs the [state machine](#pedal-openness-state-machine). (The USB queue already
   delivers CC bytes raw — no change needed in `drums_read_midi_host()`.)
3. **`src/drums.c` — the override + state + predicate** described in
   [Design](#design-cc-keyed-mode): `g_hh_open`, `is_hihat_note()`, and the `note_on()`
   override branch.
4. **Config wiring** to the runtime configurator for the six keys above (or
   compile-time defines in the Phase-1 stopgap).

No change to: the packet layout, `update_drum_state_with_midi_input`, the FreeRTOS task
topology, or relay-mode behavior.

## Implementation plan / phasing
- **Phase 0 — CC plumbing (no behavior change).** Add ControlChange parsing in `midi.c`
  and a CC branch in `drum_task` that, for now, only logs (`OPENRB_DEBUG`) the
  controller/value. Lets us confirm *which* CC the target kit actually sends for the
  pedal, and its polarity, on real hardware. This de-risks the biggest open question.
- **Phase 1 — CC-keyed mode behind a compile-time flag.** Add `g_hh_open`, the state
  machine, `is_hihat_note`, and the `note_on()` override, gated by `#define`d defaults
  (mode/cc/threshold/hyst/invert). Validate open vs. closed on hardware.
- **Phase 2 — runtime config.** Replace the `#define`s with runtime-configurator-backed,
  persisted settings; expose the toggle and tunables. Make the hi-hat note set
  configurable.

## Risks & open questions
- **Which CC / what polarity?** CC#4 is the assumed default but is kit-specific, as is
  whether high = open or high = closed. Phase 0 logging resolves this per kit; `hihat_cc`
  and `hihat_invert` make it configurable. **Assumption:** CC#4, value low = open.
- **Is yellow/blue the right RB representation of open/closed?** This spec adopts the
  existing default-map convention (closed→yellow cymbal, open→blue cymbal) so the two
  modes agree. If the desired RB semantics differ (e.g., open hi-hat should remain
  yellow, or use a different lane), the override target needs revisiting — and the
  default map itself may want updating.
- **CC-vs-note ordering / latency.** A strike and a fresh pedal update can arrive in the
  same MIDI burst; within a `drum_task` tick they're processed in FIFO order, so a strike
  is resolved against the pedal state known *at that moment*. A pedal change that lands
  one tick after the strike could misclassify a single hit. Continuous tracking +
  hysteresis keeps this to at most one transient hit at the open/closed boundary;
  acceptable. (Worth confirming the kit sends the pedal CC slightly *before* the strike.)
- **Hi-hat note set must be explicit.** Because blue cymbal is shared with ride/crash
  (notes 51/53/59 → `OUT_CYM_BLUE`), the strike set can't be derived from output color
  and must be declared. If a kit uses non-standard hi-hat note numbers, the set must be
  reconfigured — otherwise those strikes fall through to plain relay mapping.
- **Kit variability.** Some kits never send a pedal CC, or only send it on change (no
  resting value). Initial state defaults to closed; if a kit sends no CC at all,
  CC-keyed mode degrades to "always closed (yellow)" — document this and prefer relay
  mode for such kits.
- **Serial parser scope.** Adding CC is the minimum; the parser still ignores running
  status and other channel messages. Out of scope, but adding CC narrows the gap.

## References
- [`src/drums.c`](../../src/drums.c) — `output_e` (16), `get_output_for_note` (59),
  `update_drum_state_with_midi_input` (71), `note_on` (104), `drums_read_midi_host`
  (138), `drum_task` (147).
- [`src/midi.c`](../../src/midi.c) — `serial_midi_read` (75; NoteOn-only parser).
- [`inc/midi.h`](../../inc/midi.h) — `midi_type_e` (`ControlChange = 0xB0`, defined but
  unhandled).
- [`inc/midi_map.h`](../../inc/midi_map.h) + [`inc/default_midi_mapping.tbl`](../../inc/default_midi_mapping.tbl)
  — note→output table; hi-hat convention (closed→yellow, open→blue cymbal).
- [`inc/xbox_one_protocol.h`](../../inc/xbox_one_protocol.h) — `xb_one_drum_input_pkt_t`
  bitfields (`cymbal_yellow` 157, `cymbal_blue` 154).
- [`inc/adapter.h`](../../inc/adapter.h) — `VELOCITY_THRESH`, `TRIGGER_HOLD_MS`,
  `ADAPTER_OUT_INTERVAL`.
- [`inc/app_queues.h`](../../inc/app_queues.h) — `midi_note_t` / `midi_note` queue
  (carries raw 3-byte messages, CC included).
- [docs/FREERTOS-PORT.md](../FREERTOS-PORT.md) — task topology and the MIDI data path.
- [runtime-configurator](runtime-configurator.md) — persisted-settings mechanism
  (companion spec).
