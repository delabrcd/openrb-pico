#!/usr/bin/env bash
# Read the firmware's debug UART. The `monitor` compose service owns the tty and
# appends to .mon/uart.log across resets, so this just reads that log -- there's
# nothing to time against a reset.
#
#   scripts/uart.sh         # follow the log live (Ctrl-C to stop)
#   scripts/uart.sh 200     # print the last 200 lines and exit
#
# To capture a boot: `scripts/reset.sh` (writes a RESET marker), then read here.
# (`docker compose logs -f monitor` also streams it.)
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ensure_monitor   # make sure the monitor is up and the log exists

if [[ -n "${1:-}" ]]; then
    tail -n "$1" "${UART_LOG}"
else
    echo ">> following ${UART_LOG} (Ctrl-C to stop) ..." >&2
    tail -n 50 -f "${UART_LOG}"
fi
