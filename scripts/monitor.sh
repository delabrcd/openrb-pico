#!/usr/bin/env bash
# Lifecycle control for the `monitor` compose service (the UART daemon). You rarely
# need this: it auto-starts whenever any script touches the debug side, and
# scripts/uart.sh reads its log. Use this to restart/stop/rebuild it explicitly.
#
#   scripts/monitor.sh start     # ensure the monitor is up (default)
#   scripts/monitor.sh stop      # stop the monitor service
#   scripts/monitor.sh restart   # restart it
#   scripts/monitor.sh rebuild   # rebuild the image (after a Dockerfile change) + restart
#   scripts/monitor.sh status    # service state
#   scripts/monitor.sh logs      # follow the container's stdout
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

case "${1:-start}" in
    start)   ensure_monitor; echo "monitor up -> ${UART_LOG}" ;;
    stop)    compose stop monitor ;;
    restart) compose restart monitor ;;
    rebuild) compose up -d --build monitor; echo "rebuilt -> ${UART_LOG}" ;;
    status)  compose ps monitor ;;
    logs)    compose logs -f monitor ;;
    *) echo "usage: $0 {start|stop|restart|rebuild|status|logs}" >&2; exit 2 ;;
esac
