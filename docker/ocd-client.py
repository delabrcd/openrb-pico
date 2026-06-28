#!/usr/bin/env python3
"""Minimal OpenOCD TCL-RPC client for the persistent debug daemon (dbgd).

Sends $OCD_CMD to $OCD_HOST:$OCD_PORT and prints the reply. The TCL-RPC framing
byte is 0x1a (terminates both the command and the reply). Commands are wrapped in
`capture { ... }` so the human-readable console output of e.g. `program` / `reset`
comes back as the reply (a bare TCL-RPC reply is only the command's return value,
which for those commands is empty). Set OCD_RAW=1 to skip the capture wrapper.

Used by scripts/ocd.sh (and through it flash.sh / reset.sh) so every openocd
command lands in the ONE daemon-owned openocd instead of spawning a competitor
that would collide on the CMSIS-DAP probe.
"""
import os
import socket
import sys

host = os.environ.get("OCD_HOST", "dbgd")
port = int(os.environ.get("OCD_PORT", "6666"))
cmd = os.environ.get("OCD_CMD", "")
raw = os.environ.get("OCD_RAW", "0") == "1"
SEP = b"\x1a"

if not cmd.strip():
    print("!! OCD_CMD is empty", file=sys.stderr)
    sys.exit(2)

payload = cmd if raw else "capture {%s}" % cmd

try:
    sock = socket.create_connection((host, port), timeout=10)
except OSError as e:
    print("!! cannot reach openocd daemon at %s:%d (%s)\n"
          "   is it up?  scripts/dbgd.sh status" % (host, port, e),
          file=sys.stderr)
    sys.exit(1)

sock.sendall(payload.encode() + SEP)

buf = b""
sock.settimeout(60)
try:
    while SEP not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
except socket.timeout:
    print("!! timed out waiting for openocd reply", file=sys.stderr)
finally:
    sock.close()

out = buf.split(SEP, 1)[0].decode(errors="replace")
if out.strip():
    print(out)
