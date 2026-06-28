#!/usr/bin/env bash
# Send arbitrary openocd/TCL command(s) to the persistent debug daemon (dbgd)
# instead of spawning a competing openocd that would collide on the SWD probe.
# Talks to the daemon's TCL-RPC port (:6666) over the compose network and prints
# the command's console output (it's run inside `capture { ... }`).
#
#   scripts/ocd.sh 'targets'                 # list the two core targets + state
#   scripts/ocd.sh 'reset run'               # reset + run (what reset.sh sends)
#   scripts/ocd.sh 'rp2040.core0 curstate'   # is core0 halted or running?
#   scripts/ocd.sh 'reg pc'                  # current target's PC
#
# Multiple commands: separate with newlines or ';'. flash.sh / reset.sh use the
# same path (ocd_run in common.sh). For a raw reply (no capture wrapper) export
# OCD_RAW=1.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

cmd="${*:-}"
if [[ -z "${cmd}" ]]; then
    echo "usage: $0 '<openocd/tcl command>'" >&2
    exit 2
fi

ocd_run "${cmd}"
