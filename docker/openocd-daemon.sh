#!/usr/bin/env bash
# Entrypoint for the `dbgd` compose service: own the CMSIS-DAP probe and keep ONE
# persistent openocd running, so agents never manage openocd lifetime or fight over
# the probe. Mirrors docker/uart-monitor.sh: runs in the foreground as the
# container's main process (compose `restart:` supervises it) with an inner loop
# that re-launches openocd when the probe briefly drops / re-enumerates (e.g. across
# a target reset).
#
# openocd keeps four ports up (bindto 0.0.0.0 so other compose containers reach it):
#   :3333  gdb server, rp2040.core0   (scripts/gdb.sh)
#   :3334  gdb server, rp2040.core1   (scripts/gdb.sh)
#   :4444  telnet command port        (human use)
#   :6666  TCL-RPC command port       (scripts/ocd.sh -> flash.sh / reset.sh)
#
# FreeRTOS awareness: each core target is configured `-rtos auto`. If openocd can
# resolve the FreeRTOS symbols, gdb enumerates tasks as threads; if not (the SMP
# RP2040 port is not always understood by openocd 0.12), it silently disables RTOS
# for that core and gdb still gives a raw per-core backtrace. Either way the daemon
# stays up -- see docs/DEBUGGING.md.
#
# Note the empty `gdb-detach` event: it stops openocd from auto-resuming a core when
# gdb disconnects, so scripts/gdb.sh controls halt/resume explicitly (its
# --no-resume / --hold can actually leave a core halted).
set -u

ADAPTER_SPEED="${ORB_ADAPTER_SPEED:-4000}"
GDB_PORT="${ORB_GDB_PORT:-3333}"
TELNET_PORT="${ORB_TELNET_PORT:-4444}"
TCL_PORT="${ORB_TCL_PORT:-6666}"

while true; do
    echo ">> [openocd-daemon] starting openocd" \
         "(gdb core0=:${GDB_PORT} core1=:$((GDB_PORT + 1))" \
         "telnet=:${TELNET_PORT} tcl=:${TCL_PORT})" >&2
    openocd \
        -f interface/cmsis-dap.cfg \
        -c "adapter speed ${ADAPTER_SPEED}" \
        -f target/rp2040.cfg \
        -c "bindto 0.0.0.0" \
        -c "gdb_port ${GDB_PORT}" \
        -c "telnet_port ${TELNET_PORT}" \
        -c "tcl_port ${TCL_PORT}" \
        -c "gdb_memory_map disable" \
        -c "gdb_flash_program disable" \
        -c "rp2040.core0 configure -rtos auto -event gdb-detach { }" \
        -c "rp2040.core1 configure -rtos auto -event gdb-detach { }" \
        -c "init" \
        || true
    echo ">> [openocd-daemon] openocd exited; reconnecting in 1s ..." >&2
    sleep 1
done
