#!/usr/bin/env bash
# Flash a built ELF over SWD, routed through the persistent debug daemon (dbgd) so
# it never collides with the daemon's openocd on the probe (only one openocd can own
# the CMSIS-DAP probe). The flash command is sent to the SAME running openocd via its
# TCL-RPC port -- no second openocd is spawned, so you never manage openocd lifetime.
#
#   scripts/flash.sh            # flash the default board ($ORB_BOARD, CUSTOM_REV_0_1)
#   scripts/flash.sh FEATHER    # flash a specific board target
#
# Both cores are halted before programming -- core1 otherwise interferes with the
# flash algorithm (it runs the USB host loop). `program ... verify reset` then
# reflashes, verifies, and resets+runs.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

board="${1:-${BOARD}}"
# ELF lives in the optimized build/ by default; set ORB_BUILD_DIR=build-debug to flash
# the -Og deep-backtrace variant (scripts/build.sh debug). Default unchanged.
build_dir="${ORB_BUILD_DIR:-build}"
elf="${build_dir}/openrb-pico_${board}.elf"
if [[ ! -f "${REPO}/${elf}" ]]; then
    echo "!! ${elf} not found -- run scripts/build.sh first" >&2
    exit 1
fi

mark_log "FLASH ${elf} $(date -u +%Y-%m-%dT%H:%M:%SZ)"   # also ensures the monitor is up

echo ">> flashing ${elf} via debug daemon (dbgd) ..."
out="$(ocd_run "targets rp2040.core1; catch {halt}; \
targets rp2040.core0; catch {halt}; \
program ${elf} verify reset")"
printf '%s\n' "${out}"

if ! grep -q "Verified OK" <<<"${out}"; then
    echo "!! flash did not report 'Verified OK' -- check the output above." >&2
    exit 1
fi

# If openocd reports `Unknown flash device (ID 0x00ffffff)`, the QSPI flash is
# wedged in continuous-read/QPI mode -- SWD can't reset the external chip.
# Recover with a power-cycle, or hold BOOTSEL while plugging in.
#
# If the daemon's openocd is ever wedged, `scripts/dbgd.sh restart` re-homes it.
