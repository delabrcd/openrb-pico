#!/usr/bin/env bash
# Dump fuller debug state from BOTH RP2040 cores via the persistent debug daemon
# (dbgd), then leave the target running. Connects gdb (inside the build image, which
# has the toolchain + the built ELF) to the daemon's gdbservers -- :3333 core0,
# :3334 core1 -- halts each core to read, then resumes it on exit. You never manage
# openocd: the daemon owns the probe; this just connects and disconnects cleanly.
#
#   scripts/gdb.sh                 # BOTH cores: info threads, thread apply all bt, registers
#   scripts/gdb.sh bt              # BOTH cores: backtraces only (thread apply all bt)
#   scripts/gdb.sh regs            # BOTH cores: info registers
#   scripts/gdb.sh tasks           # ALL FreeRTOS tasks: name+state+backtrace (SMP walker)
#   scripts/gdb.sh core0 "<cmd>"   # run an arbitrary gdb command against core0 only
#   scripts/gdb.sh core1 "<cmd>"   # ... against core1 only
#
# `tasks` walks the FreeRTOS task lists (docker/freertos-tasks.py) against core0's
# gdbserver to enumerate EVERY task (openocd's built-in -rtos FreeRTOS can't, because
# the SMP RP2040 port uses pxCurrentTCBs[] not pxCurrentTCB), then separately dumps
# core1's live backtrace (core1's running task lives in that CPU, not RAM). See
# docs/DEBUGGING.md.
#
# Flags (before the subcommand):
#   --no-resume / --hold           # leave the core(s) HALTED instead of resuming
#
# FreeRTOS: each core is configured `-rtos auto` in the daemon, so `info threads` /
# `thread apply all bt` enumerate FreeRTOS tasks IF openocd resolves them; otherwise
# you get the raw per-core backtrace (still useful). See docs/DEBUGGING.md.
#
# Board: ELF is <build dir>/openrb-pico_${ORB_BOARD}.elf (default CUSTOM_REV_0_1).
# Build dir defaults to build/; set ORB_BUILD_DIR=build-debug to debug the -Og
# deep-backtrace variant (scripts/build.sh debug). Default unchanged.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

board="${BOARD}"
build_dir="${ORB_BUILD_DIR:-build}"
elf="${build_dir}/openrb-pico_${board}.elf"

resume=1
case "${1:-}" in
    --no-resume|--hold) resume=0; shift ;;
esac

# Default command set (joined with '|||', split again in docker/gdb-dump.py).
DEFAULT_CMDS='info threads|||thread apply all bt|||info registers'

sub="${1:-all}"
declare -a cores
cmds="${DEFAULT_CMDS}"
case "${sub}" in
    all|"")  cores=(0 1) ;;
    bt)      cores=(0 1); cmds='thread apply all bt' ;;
    regs)    cores=(0 1); cmds='info registers' ;;
    tasks)   ;;   # handled specially below (FreeRTOS SMP task walker)
    core0)   cores=(0); shift; cmds="${1:-${DEFAULT_CMDS}}" ;;
    core1)   cores=(1); shift; cmds="${1:-${DEFAULT_CMDS}}" ;;
    *) echo "usage: $0 [--no-resume] {|bt|regs|tasks|core0 \"<cmd>\"|core1 \"<cmd>\"}" >&2; exit 2 ;;
esac

if [[ ! -f "${REPO}/${elf}" ]]; then
    echo "!! ${elf} not found -- run scripts/build.sh first" >&2
    exit 1
fi

ensure_dbgd

ports=("${DBGD_GDB_CORE0}" "${DBGD_GDB_CORE1}")

# `tasks`: enumerate EVERY FreeRTOS task (SMP walker on core0), then dump core1's live
# backtrace so the core1-pinned host task's *current* stack is shown too.
if [[ "${sub}" == "tasks" ]]; then
    echo ""
    echo "################  FreeRTOS tasks  (walk via core0 gdbserver :${DBGD_GDB_CORE0})  ################"
    compose run --rm --no-deps -T \
        -e GDB_HOST=dbgd -e GDB_PORT="${DBGD_GDB_CORE0}" -e GDB_CORE=0 \
        -e GDB_RESUME="${resume}" \
        build gdb-multiarch -q -nx -batch \
            -x /work/docker/freertos-tasks.py \
            "${elf}"
    echo ""
    echo "################  core1 live backtrace  (gdbserver :${DBGD_GDB_CORE1})  ################"
    compose run --rm --no-deps -T \
        -e GDB_HOST=dbgd -e GDB_PORT="${DBGD_GDB_CORE1}" \
        -e GDB_RESUME="${resume}" -e GDB_CMDS='thread apply all bt' \
        build gdb-multiarch -q -nx -batch \
            -x /work/docker/gdb-dump.py \
            "${elf}"
    if [[ "${resume}" -eq 0 ]]; then
        echo ""
        echo ">> NOTE: core(s) left HALTED (--no-resume). Resume with:" \
             "scripts/ocd.sh 'rp2040.core0 resume; rp2040.core1 resume'" >&2
    fi
    exit 0
fi
for idx in "${cores[@]}"; do
    port="${ports[${idx}]}"
    echo ""
    echo "################  core${idx}  (gdbserver :${port})  ################"
    compose run --rm --no-deps -T \
        -e GDB_HOST=dbgd -e GDB_PORT="${port}" \
        -e GDB_RESUME="${resume}" -e GDB_CMDS="${cmds}" \
        build gdb-multiarch -q -nx -batch \
            -x /work/docker/gdb-dump.py \
            "${elf}"
done

if [[ "${resume}" -eq 0 ]]; then
    echo ""
    echo ">> NOTE: core(s) left HALTED (--no-resume). Resume with:" \
         "scripts/ocd.sh 'rp2040.core0 resume; rp2040.core1 resume'  (or scripts/gdb.sh bt)" >&2
fi
