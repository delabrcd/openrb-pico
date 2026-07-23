# Contributing to OpenRB

Thanks for your interest in helping out! This page is the quick orientation for working on
the firmware. The deeper material lives elsewhere:

- **How the code is organized** → the [Architecture](https://github.com/delabrcd/openrb-pico/wiki/Architecture)
  page on the wiki (layered C++23 design + the FreeRTOS-SMP runtime model).
- **The rules your code must follow** → [`AGENTS.md`](AGENTS.md) — the short, *enforceable*
  list (static DI, no heap, layering, core1 concurrency).
- **Build / flash / debug** → [`BUILDING.md`](BUILDING.md) and the [`scripts/`](scripts/) helpers.

## Development setup

Everything builds in Docker — the only host requirement is Docker (Compose v2). See
[`BUILDING.md`](BUILDING.md) for details.

```sh
scripts/build.sh                                       # both board targets, logging on
cmake --preset release && cmake --build build-release  # release: logging compiled out
```

## Before you open a PR

1. **Build both profiles.** `scripts/build.sh` (default) and the `release` preset above.
   CI ([`.github/workflows/firmware.yml`](.github/workflows/firmware.yml)) builds the
   release preset inside the pinned dev-container image, so match that locally.
2. **Run the host tests** for the portable logic (protocol layer, doctest):
   ```sh
   cmake -S test -B test/build && cmake --build test/build && ./test/build/protocol_tests
   ```
3. **Lint changed files** (fast clang-tidy, no full firmware build):
   ```sh
   scripts/check.sh            # git-changed .cpp/.h/.hpp under modules/ + test/
   ```
4. **Respect the portability boundary.** `scripts/check-boundary.sh` runs in CI and fails
   if `modules/service/` or `modules/protocol/` pick up SDK / RTOS / TinyUSB includes, or
   if the deleted DI forwarders reappear. Keep vendor/TinyUSB code in `driver/` and
   pico-sdk code in `platform/`.

## Coding conventions

- **C++23**, no heap, static dependency injection, one composition root
  (`modules/app/system.cpp`). The specifics — and *why* — are in
  [`AGENTS.md`](AGENTS.md) and the [Architecture](https://github.com/delabrcd/openrb-pico/wiki/Architecture) page.
- **Match the surrounding code** — its naming, comment density, and idiom.
- **Mind core1.** The PIO-USB host task is timing-critical: no `sleep_ms`/`board_millis`,
  no blocking/unbounded mutexes, no non-relaxed atomics on it. When in doubt, see the core1
  rules in `AGENTS.md`.

## Submitting changes

- Branch off `main` and open a pull request — `main` is protected and only takes changes
  through a PR. CI must build.
- Keep commits focused; describe *what changed and why* in the body.
- Firmware behavior changes should be verified on hardware where practical — a physical
  power-cycle + the board LED are the ground truth for USB enumeration (SWD `reset` is not
  equivalent). See the caveats in [`BUILDING.md`](BUILDING.md).

## Boards

Changes should keep **both** board targets building — `CUSTOM_REV_0_1` (the OpenRB PCB) and
`FEATHER` (Adafruit Feather RP2040 USB Host). Board-specific behavior belongs in
`modules/board/`.
