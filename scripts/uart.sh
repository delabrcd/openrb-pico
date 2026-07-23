#!/usr/bin/env bash
# Read the firmware's debug UART. The `monitor` compose service owns the tty and
# appends to .mon/uart.log across resets, so this just reads that log -- there's
# nothing to time against a reset.
#
#   scripts/uart.sh         # follow the log live (Ctrl-C to stop)
#   scripts/uart.sh 200     # print the last 200 lines and exit
#
# The firmware logs PLAIN text ([ts][LEVEL][core][CAT] msg); color is applied HERE, at
# view time, and ONLY when stdout is a terminal -- so the live view is colorized by log
# level while piping/redirecting (grep, capture) stays plain. Any stray SGR already in
# the stream is stripped first, so this is correct even if the firmware was built with
# ORB_LOG_COLOR=1. To force one mode: ORB_COLOR=1 (always) / ORB_COLOR=0 (never).
#
# To capture a boot: `scripts/reset.sh` (writes a RESET marker), then read here.
# (`docker compose logs -f monitor` also streams it.)
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ensure_monitor   # make sure the monitor is up and the log exists

# Decide whether to colorize: explicit ORB_COLOR wins, else auto (on iff stdout is a tty).
if [[ "${ORB_COLOR:-auto}" == "1" ]]; then COLOR=1
elif [[ "${ORB_COLOR:-auto}" == "0" ]]; then COLOR=0
elif [[ -t 1 ]]; then COLOR=1
else COLOR=0
fi

# Strip any existing SGR, then wrap each line per its [LEVEL] field: ERR red, WARN
# yellow, DEBUG/TRACE dim, INFO default. fflush keeps the live follow responsive.
colorize() {
    awk '{
        gsub(/\033\[[0-9;]*m/, "");
        if ($0 ~ /\]\[ERR  \]/)                          c="\033[31m";
        else if ($0 ~ /\]\[WARN \]/)                     c="\033[33m";
        else if ($0 ~ /\]\[DEBUG\]/ || $0 ~ /\]\[TRACE\]/) c="\033[2m";
        else                                             c="";
        if (c != "") printf "%s%s\033[0m\n", c, $0; else print $0;
        fflush();
    }'
}

emit() { if [[ "${COLOR}" -eq 1 ]]; then colorize; else cat; fi; }

if [[ -n "${1:-}" ]]; then
    tail -n "$1" "${UART_LOG}" | emit
else
    echo ">> following ${UART_LOG} (Ctrl-C to stop) ..." >&2
    tail -n 50 -f "${UART_LOG}" | emit
fi
