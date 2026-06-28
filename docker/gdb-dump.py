# GDB (python) driver for scripts/gdb.sh -- sourced via `gdb -x`.
#
# Connects to one of the daemon's gdbservers ($GDB_HOST:$GDB_PORT), halts the
# core, runs each command in $GDB_CMDS (split on '|||'), then resumes (unless
# $GDB_RESUME != 1) and detaches. The try/finally guarantees the resume runs even
# if a command errors, so a normal dump never leaves a core halted.
import os

host = os.environ.get("GDB_HOST", "dbgd")
port = os.environ.get("GDB_PORT", "3333")
resume = os.environ.get("GDB_RESUME", "1") == "1"
cmds = [c for c in os.environ.get("GDB_CMDS", "").split("|||") if c.strip()]

gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("set print pretty on")

try:
    gdb.execute("target extended-remote %s:%s" % (host, port))
except gdb.error as e:
    print("!! could not connect to %s:%s -- is the dbgd daemon up? "
          "(scripts/dbgd.sh status)  [%s]" % (host, port, e))
    gdb.execute("quit 1")  # clean nonzero exit (no Python-exception noise)

try:
    try:
        gdb.execute("monitor halt")
    except gdb.error as e:
        print("[warn] halt: %s" % e)
    for c in cmds:
        print("\n----- (gdb) %s -----" % c)
        try:
            gdb.execute(c)
        except gdb.error as e:
            print("[error] %s" % e)
finally:
    if resume:
        try:
            gdb.execute("monitor resume")
        except gdb.error as e:
            print("[warn] resume: %s" % e)
    try:
        gdb.execute("detach")
    except gdb.error:
        pass
