#!/usr/bin/env bash
# Lifecycle control for the `dbgd` compose service (the persistent SWD debug
# daemon: ONE openocd owning the probe, gdbservers on :3333/:3334, command ports
# :4444/:6666). You rarely need this: it auto-starts whenever flash.sh / reset.sh /
# gdb.sh touch the probe. Use it to restart/stop/rebuild it or watch openocd's log.
#
#   scripts/dbgd.sh start     # ensure the daemon is up (default)
#   scripts/dbgd.sh stop      # stop it (frees the probe for an external openocd)
#   scripts/dbgd.sh restart   # restart it (e.g. after a probe hotplug)
#   scripts/dbgd.sh rebuild   # rebuild the image (after a Dockerfile change) + restart
#   scripts/dbgd.sh status    # service state
#   scripts/dbgd.sh logs      # follow openocd's stdout/stderr
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

case "${1:-start}" in
    start)   ensure_dbgd; echo "dbgd up -> gdb :${DBGD_GDB_CORE0}/:${DBGD_GDB_CORE1}, telnet :${DBGD_TELNET}, tcl :${DBGD_TCL}" ;;
    stop)    compose stop dbgd ;;
    restart) compose restart dbgd ;;
    rebuild) compose up -d --build dbgd; echo "rebuilt dbgd" ;;
    status)  compose ps dbgd ;;
    logs)    compose logs -f dbgd ;;
    *) echo "usage: $0 {start|stop|restart|rebuild|status|logs}" >&2; exit 2 ;;
esac
