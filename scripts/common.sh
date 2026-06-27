#!/usr/bin/env bash
# Shared config + helpers for the openrb-pico dev scripts.
# Source this from the other scripts; do not run directly.
#
# Everything runs via docker-compose.yml (../docker-compose.yml):
#   build_run    -> one-shot in the `build` service (cross-compiler)
#   dbg_run      -> one-shot in the `dbg` service (openocd, privileged + /dev)
#   monitor      -> long-running `monitor` service owning the debug UART
#
# Override defaults via the environment, e.g. ORB_BOARD=FEATHER scripts/flash.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="${ORB_BOARD:-CUSTOM_REV_0_1}"   # or FEATHER

# UART monitor log: host path (read by uart.sh) and in-container path (mounted).
UART_LOG="${REPO}/.mon/uart.log"
UART_LOG_C="/work/.mon/uart.log"

compose() { docker compose -f "${REPO}/docker-compose.yml" "$@"; }

# Ensure the UART monitor service is up. Idempotent (no-op if already running);
# `up` builds the shared openrb-pico-dev image on first use. After editing the
# Dockerfile, rebuild with `scripts/monitor.sh rebuild` or `docker compose build`.
ensure_monitor() {
    compose up -d monitor >&2
}

# Run a command in the firmware build image (repo mounted at /work).
build_run() {
    compose run --rm build bash -lc "$*"
}

# Run a command in a one-shot privileged debug container (probe + UART present).
# Ensures the monitor is up first so flashing/resetting always lands in the log.
dbg_run() {
    ensure_monitor
    compose run --rm dbg bash -lc "cd /work && $*"
}

# Append a marker line to the UART log via the running monitor container.
mark_log() {  # <text>
    ensure_monitor
    compose exec -T monitor bash -lc \
        "printf '==================== %s ====================\n' \"\$0\" >> '${UART_LOG_C}'" "$1"
}
