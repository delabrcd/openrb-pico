#!/usr/bin/env bash
# Issue an SWD `reset run` to the target via the orb-dbg probe.
#
# WARNING: an SWD reset is NOT equivalent to the physical RESET button for this
# board's USB-wedge behaviour, and does NOT power-cycle the CH334R hub. Use it for
# quick re-runs, but treat any pass/fail enumeration result from an SWD reset as
# suspect -- ground truth is a physical reset + watching the controller LED.
# See ../docs (usb-stack-saga.md), "Critical testing methodology".
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

mark_log "RESET $(date -u +%Y-%m-%dT%H:%M:%SZ)"   # also ensures the monitor is up

echo ">> SWD reset run (boot output will land in ${UART_LOG}) ..."
dbg_run "openocd -f interface/cmsis-dap.cfg \
    -c 'adapter speed 4000' -f target/rp2040.cfg \
    -c 'init; reset run; exit'"
