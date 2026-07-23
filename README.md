# OpenRB

**Play your MIDI drum kit as a pro-drums controller on Xbox One.**

<p align="center">
  <img src="docs/img/openrb-board.png" alt="The OpenRB adapter board" width="640">
</p>

OpenRB is a small, open-source adapter that lets an electronic drum kit act as an
official-style **pro drums** controller — so you can play the drums you already own on
your console. No proprietary dongle, no fragile workarounds.

## Why OpenRB

- 🎯 **Just works on Xbox One.** First-class Xbox One support with a genuinely simple
  setup — no complicated flashing rituals or PC-side configuration to keep it working.
- 🥁 **Use the kit you have.** Connects to drum kits over USB or a standard MIDI cable.
- 🔌 **One adapter, many instruments.** Built to support multiple instrument types from a
  single adapter (guitar support is on the way).
- 🛠️ **Open and yours.** Fully open-source firmware and hardware — build it yourself or
  run it on an off-the-shelf board.

## Get started in 3 steps

You don't need to be a programmer. You'll need an OpenRB board (or an
[Adafruit Feather RP2040 USB Host](https://www.adafruit.com/product/5723)) and your drum
kit.

1. **Download** the latest firmware from the
   [Releases page](https://github.com/delabrcd/openrb-pico/releases) — pick
   `openrb-pico_FEATHER.uf2` for a Feather, or `openrb-pico_CUSTOM_REV_0_1.uf2` for the
   OpenRB board.
2. **Flash it:** hold the **BOOTSEL** button on the board, plug it into your computer with
   a USB cable, and drag the downloaded file onto the drive that appears (`RPI-RP2`). Done —
   it restarts on its own.
3. **Plug in and play:** connect your drum kit, then connect the board to your Xbox with a
   USB cable. Start the game and pick up your sticks.

👉 **Step-by-step guide with photos and wiring:**
[Feather setup on the wiki](https://github.com/delabrcd/openrb-pico/wiki/Feather-USB-Host).

## How it compares

OpenRB is in the same family as **Santroller** and the **Roll Limitless** / PDP legacy
adapter. What sets it apart is out-of-the-box **Xbox One** support with a simpler setup,
and an architecture designed to adapt *many* instrument types from one adapter (using the
Rock Band Wireless Legacy Adapter protocol).

## Learn more

- 📖 **[Project wiki](https://github.com/delabrcd/openrb-pico/wiki)** — setup guides,
  features, and how it works.
- 💬 **[Releases](https://github.com/delabrcd/openrb-pico/releases)** — download firmware.

<sub>Developers: build-from-source, architecture, and contributor docs live in the
[wiki](https://github.com/delabrcd/openrb-pico/wiki) and in
[`AGENTS.md`](AGENTS.md) · [`BUILDING.md`](BUILDING.md) ·
[`docs/architecture.md`](docs/architecture.md).</sub>
