# Running openrb-pico on an Adafruit Feather RP2040 (USB host)

How to bring up the firmware on a **Feather RP2040** instead of the custom OpenRB
board. The Feather is the `FEATHER` build target — the original bring-up target the
custom board was later derived from — so it's the easiest way to try openrb-pico with
off-the-shelf hardware and no SWD probe.

> **TL;DR:** grab `openrb-pico_FEATHER.uf2` from the
> [latest release](https://github.com/delabrcd/openrb-pico/releases), hold **BOOTSEL**
> while plugging the Feather into your PC, drag the `.uf2` onto the `RPI-RP2` drive,
> then wire your drum kit's MIDI OUT to **GPIO1** and plug the controller into the USB
> host port.

---

## 1. What you need

| | |
|---|---|
| **Board** | **Adafruit Feather RP2040 with USB Type A Host** ([product 5723](https://www.adafruit.com/product/5723)) — recommended. Its USB-A host port + 5 V load switch are already wired to the exact GPIOs the firmware expects (see §3). A *plain* Feather RP2040 works too but you must hand-wire the host port yourself (§3, caveat). |
| **Instrument** | A rhythm-game drum kit that outputs **serial MIDI** (the adapter reads the kit as a MIDI instrument). |
| **Cables** | USB-C (Feather ↔ console/PC, the *device* side) + whatever your controller/kit use on the *host* side. |
| **Optional** | A USB-serial adapter on the debug UART (§6) if you want boot logs — only useful with a logging build. |

The RP2040's **native USB** is the *device* side (what the console/PC sees as an Xbox
pro-drum adapter). A **second, bit-banged USB host** is synthesized in PIO
([Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)) on GPIO16/17 — that's
where your controller plugs in. The two are independent.

---

## 2. Get the firmware

**Option A — download (fastest).** From the
[releases page](https://github.com/delabrcd/openrb-pico/releases), download
`openrb-pico_FEATHER.uf2`. This is the **release** build: logging is compiled out and
the low-latency timing is baked in.

**Option B — build from source.** Everything runs in Docker (see
[`BUILDING.md`](../BUILDING.md)):

```sh
scripts/build.sh          # builds BOTH board targets into build/
# release variant (logging compiled out) into build-release/:
cmake --preset release && cmake --build build-release -j"$(nproc)"
```

Board selection is a compile-time define (`ORB_BOARD_ID`), not a runtime switch — every
configure builds both targets, so you just pick the right artifact:

- `build-release/openrb-pico_FEATHER.uf2` — release, no logging (ship this).
- `build/openrb-pico_FEATHER.uf2` — default build, **UART logging on** (use this for
  bring-up so you can watch the boot; see §6).

---

## 3. Pinout (Feather target)

Defined in [`modules/board/pins_rp2040_usbh.h`](../modules/board/pins_rp2040_usbh.h);
actuation in [`modules/board/actuators.cpp`](../modules/board/actuators.cpp).

| Function | GPIO | Notes |
|---|---|---|
| **USB host D+** | **GPIO16** | PIO-USB. `pin_usb_host_dp` |
| **USB host D-** | **GPIO17** | PIO-USB requires D- = D+ + 1 |
| **USB host 5 V enable** | **GPIO18** | firmware-driven VBUS/5 V switch (`pin_5v_en`) — Feather only |
| Serial-MIDI RX (from kit's MIDI OUT) | **GPIO1** | `uart0`, **31250 baud** |
| Serial-MIDI TX | GPIO0 | `uart0` |
| Debug UART TX | GPIO24 | `uart1`, 115200 baud |
| Debug UART RX | GPIO25 | `uart1` |
| Status LED | **GPIO13** | `pin_led` |

On the **Feather RP2040 USB Host** variant, GPIO16/17/18 already go to the on-board
USB-A host receptacle and its 5 V load switch — nothing to wire for USB.

> **Hand-wiring a *plain* Feather RP2040 (advanced, undocumented):** solder a USB-A
> receptacle with **D+ → GPIO16, D- → GPIO17, VBUS → 5 V (gated by GPIO18), GND → GND**.
> The repo does not document pull-up/series-resistor values for a hand-wired host port —
> the tested topology is the USB-Host Feather variant. Proceed at your own risk.

---

## 4. Flash it (BOOTSEL / UF2 — no probe needed)

A plain Feather has no SWD header, so use the RP2040 UF2 bootloader:

1. Hold the **BOOTSEL** button while plugging the Feather's **USB-C** into your PC.
2. It mounts as a mass-storage drive named **`RPI-RP2`**.
3. Drag **`openrb-pico_FEATHER.uf2`** onto that drive.
4. The Feather reboots automatically into the firmware.

> The `scripts/flash.sh` helper flashes over **SWD** and defaults to `CUSTOM_REV_0_1`;
> it's only relevant if you've wired a CMSIS-DAP probe. For a Feather, UF2 drag-drop
> above is the path.

---

## 5. Wire the drum kit + controller

- **Drum kit → Feather:** connect the kit's **MIDI OUT** to the Feather's serial-MIDI
  **RX = GPIO1** (and GND). The adapter reads it at **31250 baud** (standard MIDI) — see
  [`modules/service/midi.cpp`](../modules/service/midi.cpp). If your kit uses a 5-pin
  DIN MIDI port, use a standard MIDI-DIN → UART opto-isolated front end; if it exposes
  TTL serial, wire it directly (3.3 V-safe only).
- **Controller → Feather:** plug the Xbox controller into the **USB-A host port**
  (GPIO16/17). On the USB-Host Feather variant you can connect it **directly** — the
  CH334R hub that the custom board hard-wires is **not** required here. (A hub is
  optional if you want to attach extra USB devices.)
- **Feather → console/PC:** the **USB-C** (native USB) presents as the pro-drum adapter.

---

## 6. First boot & verify

- The **LED on GPIO13** is the enumeration indicator — LED on ≈ the controller
  enumerated through the host stack.
- **Release build:** logging is compiled out, so the debug UART is intentionally silent —
  a quiet UART is expected and is *not* a failure signal.
- **Want boot logs?** Flash the default (non-release) build
  (`build/openrb-pico_FEATHER.uf2`) and attach a USB-serial adapter to the **debug UART
  (GPIO24 TX / GPIO25 RX, 115200 baud)**. A healthy boot ends with the Xbox One
  handshake (`CMD_POWER_MODE`, `CMD_LED_MODE`, `CMD_AUTHENTICATE`) — see
  [`BUILDING.md`](../BUILDING.md).

The CPU runs at **120 MHz** on both boards
([`modules/app/main.cpp`](../modules/app/main.cpp) — `set_sys_clock_khz(120000, …)`);
there's no Feather-specific clock change.

---

## 7. How the Feather differs from the custom OpenRB board

| | **FEATHER** | **CUSTOM_REV_0_1** |
|---|---|---|
| GPIO18 | firmware-driven **5 V enable** (`set_usb_host()` toggles it) | **CH334R hub RESET#** (`reset_usb_hub()`) — *not* a 5 V enable |
| Downstream USB power | firmware can cut/restore VBUS via GPIO18 | VBUS is hard-wired +5 V (can't be gated in firmware) |
| USB hub | optional; controller can be direct | CH334R hub is in the signal path (hardwired) |
| LED | GPIO13 | GPIO23 |

This is why the custom board's **warm-reset / hub-wedge recovery** machinery (a whole
class of "power-cycle the hub to un-wedge it" behaviour) doesn't apply to a Feather: the
Feather can just toggle GPIO18 to power-cycle its downstream port, and with the
controller plugged in directly there's no CH334R to accumulate bad state. Background on
the hub/clock/PIO-USB choices lives in
[`../../docs/usb-stack-saga.md`](../../docs/usb-stack-saga.md) and
[`PORTING.md`](../PORTING.md).

---

## 8. Troubleshooting

- **`RPI-RP2` drive never appears** — you didn't catch BOOTSEL; unplug, hold BOOTSEL
  *before* plugging in, keep holding until it mounts.
- **Controller doesn't enumerate (LED stays off)** — confirm it's on the **host** port
  (GPIO16/17), not the native USB-C; power-cycle the whole board; verify your kit/console
  actually powers the host port (GPIO18 must be able to source 5 V to the device).
- **No drum hits register** — check MIDI OUT → **GPIO1 (RX)** and GND, and that the kit
  sends on standard 31250-baud MIDI. Flash the logging build (§6) to watch the parser.
- **Timing feels off / notes drop or double** — the release build ships aggressive
  low-latency values (`trigger_hold` 15 ms, `on_delay` 5 ms in
  [`modules/service/adapter.h`](../modules/service/adapter.h)); raise them and rebuild if
  your setup needs more margin.

---

## References

- [`BUILDING.md`](../BUILDING.md) — build / flash / debug workflow (Docker).
- [`modules/board/pins_rp2040_usbh.h`](../modules/board/pins_rp2040_usbh.h) — the Feather pin map.
- [`modules/board/actuators.cpp`](../modules/board/actuators.cpp) — the GPIO18 5 V-enable vs hub-reset split.
- [`PORTING.md`](../PORTING.md) / [`../../docs/usb-stack-saga.md`](../../docs/usb-stack-saga.md) — PIO-USB / clock / hub rationale.
- [Adafruit Feather RP2040 USB Host (5723)](https://www.adafruit.com/product/5723).
