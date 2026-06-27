#!/usr/bin/env bash
# Flash a built ELF over SWD via the `dbg` compose service (CMSIS-DAP probe).
#
#   scripts/flash.sh            # flash the default board ($ORB_BOARD, CUSTOM_REV_0_1)
#   scripts/flash.sh FEATHER    # flash a specific board target
#
# Both cores are halted before programming -- core1 otherwise interferes with the
# flash algorithm (it runs the USB host loop).
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

board="${1:-${BOARD}}"
elf="build/openrb-pico_${board}.elf"
if [[ ! -f "${REPO}/${elf}" ]]; then
    echo "!! ${elf} not found -- run scripts/build.sh first" >&2
    exit 1
fi

echo ">> flashing ${elf} ..."
dbg_run "openocd -f interface/cmsis-dap.cfg \
    -c 'adapter speed 4000' -f target/rp2040.cfg \
    -c 'init' \
    -c 'targets rp2040.core1' -c 'catch {halt}' \
    -c 'targets rp2040.core0' -c 'catch {halt}' \
    -c 'program ${elf} verify reset exit'"

# If openocd reports `Unknown flash device (ID 0x00ffffff)`, the QSPI flash is
# wedged in continuous-read/QPI mode -- SWD can't reset the external chip.
# Recover with a power-cycle, or hold BOOTSEL while plugging in.
