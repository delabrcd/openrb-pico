#!/usr/bin/env bash
# Entrypoint for the `monitor` compose service: own the debug UART and append
# timestamped lines to the log, reconnecting across target/probe resets. Runs in
# the foreground as the container's main process (compose `restart:` supervises it);
# the inner loop re-opens the tty when the probe briefly re-enumerates.
#
# Output is tee'd to both the log file (read by scripts/uart.sh) and stdout
# (visible via `docker compose logs -f monitor`).
set -u

TTY="${ORB_TTY:-/dev/ttyACM0}"
BAUD="${ORB_BAUD:-115200}"
LOG="${ORB_LOG:-/work/.mon/uart.log}"

mkdir -p "$(dirname "${LOG}")"

{
    while true; do
        stty -F "${TTY}" "${BAUD}" raw -echo 2>/dev/null || true
        cat "${TTY}" 2>/dev/null | gawk '@load "time";
            { t = gettimeofday(); ms = int((t - int(t)) * 1000);
              printf "[%s.%03d] %s\n", strftime("%H:%M:%S", int(t)), ms, $0;
              fflush(); }' 2>/dev/null
        sleep 0.5
    done
} | tee -a "${LOG}"
