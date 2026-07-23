#!/usr/bin/env bash
# Issue an SWD `reset run` to the target via the orb-dbg probe.
#
# WARNING: an SWD reset is NOT equivalent to the physical RESET button for this
# board's USB-wedge behaviour, and does NOT power-cycle the CH334R hub. Use it for
# quick re-runs, but treat any pass/fail enumeration result from an SWD reset as
# suspect -- ground truth is a physical reset + watching the controller LED.
# See ../docs (usb-stack-saga.md), "Critical testing methodology".
#
# Routed through the persistent debug daemon (dbgd): the `reset run` is sent to the
# SAME running openocd via its TCL-RPC port, so it never spawns a competing openocd
# or fights for the probe.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

mark_log "RESET $(date -u +%Y-%m-%dT%H:%M:%SZ)"   # also ensures the monitor is up

echo ">> SWD reset run via debug daemon (boot output will land in ${UART_LOG}) ..."
ocd_run "reset run"
