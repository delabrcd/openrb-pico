# On-target debugging: the `dbgd` daemon + dual-core backtraces

When the firmware hangs (e.g. a dual-core lock-up), you want **full backtraces from
both RP2040 cores** without hand-spawning openocd — repeatedly starting/stopping
openocd is what wedges the CMSIS-DAP probe. The `dbgd` compose service solves this:
**one persistent openocd** owns the probe and keeps the gdbservers + command ports
up. Agents just connect, dump, disconnect. You never manage openocd lifetime.

This mirrors the `monitor` service (which owns the debug UART). See
[`BUILDING.md`](../BUILDING.md) for the build/flash basics.

## Port map

One openocd, four ports (bound `0.0.0.0` inside the container so other compose
containers reach it as `dbgd:<port>`; also published to `127.0.0.1` on the host):

| port | what | used by |
|------|------|---------|
| `:3333` | gdb server, `rp2040.core0` | `scripts/gdb.sh` |
| `:3334` | gdb server, `rp2040.core1` | `scripts/gdb.sh` |
| `:4444` | openocd **telnet** command port | humans (`telnet localhost 4444`) |
| `:6666` | openocd **TCL-RPC** command port | `scripts/ocd.sh` → `flash.sh`/`reset.sh` |

## Quick start

```sh
scripts/dbgd.sh start        # bring the daemon up (auto-starts anyway; idempotent)
scripts/gdb.sh               # full dual-core dump (threads + backtraces + registers)
```

`scripts/gdb.sh` runs gdb (in the build image, which has the toolchain + the built
ELF), halts each core to read it, then **resumes it on exit** — the target is left
running. Default output, for BOTH cores: `info threads`, `thread apply all bt`,
`info registers`.

Subcommands:

```sh
scripts/gdb.sh bt                  # backtraces only, both cores
scripts/gdb.sh regs                # registers only, both cores
scripts/gdb.sh core0 "p/x $pc"     # arbitrary gdb command against core0 only
scripts/gdb.sh core1 "bt full"     # ... against core1 only
scripts/gdb.sh --no-resume         # dump, but leave the core(s) HALTED (default resumes)
```

`ORB_BOARD` selects the ELF (`build/openrb-pico_${ORB_BOARD}.elf`, default
`CUSTOM_REV_0_1`). The daemon must have been built/flashed with the matching firmware
for the symbols to line up.

### Example (a real dual-core capture)

```
################  core0  (gdbserver :3333)  ################
----- (gdb) thread apply all bt -----
Thread 1 (Remote target):
#0  critical_section_enter_blocking (...) at .../sync/spin_lock.h:304
#1  osal_queue_receive (...) at .../osal/osal_pico.h:154
#2  tud_task_ext (...) at .../device/usbd.c:651
################  core1  (gdbserver :3334)  ################
----- (gdb) thread apply all bt -----
Thread 1 (Remote target):
#0  0x1005ed34 in ?? ()
#1  <signal handler called>
   ... psp -> s_usb_host_task (the core1 USB host loop)
```

core0 runs the TinyUSB **device** task; core1 runs the PIO-USB **host** task — a hang
shows immediately where each core is stuck.

## FreeRTOS thread awareness (and its limitation)

Each core target is configured `-rtos auto`, so **if** openocd can resolve the
FreeRTOS task list, `info threads` / `thread apply all bt` enumerate tasks as gdb
threads. **In practice this does not work for this firmware:** openocd 0.12's
FreeRTOS RTOS module looks for the single-core `pxCurrentTCB` symbol, but the
SMP RP2040 port (FreeRTOS-Kernel V11.2.0, `portable/ThirdParty/GCC/RP2040`) uses a
per-core `pxCurrentTCBs[]` **array** instead — so auto-detect reports:

```
Warn : No RTOS could be auto-detected!
```

and openocd falls back to plain per-core debugging. That fallback is **fine**: you
still get the full backtrace of whatever each core is currently executing (which is
exactly what you need for a hang), just keyed by core rather than by task. The
`-rtos auto` config is left in place so the feature lights up automatically if a
future openocd/FreeRTOS combination supports the SMP port — it never breaks the
daemon (auto-detect failure is non-fatal, and the daemon's reconnect loop is robust
to it regardless).

### `scripts/gdb.sh tasks` — the SMP task walker

To get **every** task (not just the two currently running), use:

```sh
scripts/gdb.sh tasks
```

This runs [`docker/freertos-tasks.py`](../docker/freertos-tasks.py), a gdb-python helper
that does what openocd's RTOS module can't here: it connects to **one** gdbserver
(core0 — RAM is shared between cores), walks the FreeRTOS task lists in memory
(`pxReadyTasksLists`, the delayed/pending/suspended lists, `pxCurrentTCBs[]`), and for
each task prints its **name, priority, state, and a backtrace**:

- **Running on core0** (the connected core): live registers are correct → live `bt`.
- **Blocked / ready / suspended** (not running): the helper reconstructs the saved CPU
  context from the task's `pxTopOfStack` using the RP2040 port's PendSV stack-frame
  layout (8 software-saved low regs `r4–r11`, then the hardware exception frame
  `r0–r3,r12,lr,pc,xpsr`), temporarily sets gdb's registers, runs `bt`, and restores.
- **Running on core1** (the host task, pinned there): its live registers live in
  *core1's* CPU, not in RAM, so the walker can't read them from core0's gdbserver. It
  flags this, and `scripts/gdb.sh tasks` then **separately dumps core1's live
  backtrace** from the `:3334` gdbserver so you still see the host loop's current stack.

The backtraces are much deeper if you flash the **`-Og` debug build** first (see
[`../BUILDING.md`](../BUILDING.md) → *Deep-backtrace debug build*):

```sh
scripts/build.sh debug
ORB_BUILD_DIR=build-debug scripts/flash.sh
ORB_BUILD_DIR=build-debug scripts/gdb.sh tasks
```

Expect to see roughly: the core1-pinned **usb_host** task, the **core0** application
tasks, **Tmr Svc** (the timer-service daemon), and **IDLE0 / IDLE1** (one idle task per
core, flagged `[idle]`). Names come from `pcTaskName`; states from which list the TCB
was found on. To inspect by hand instead, the kernel symbols are loaded, so
`p pxCurrentTCBs`, `p pxReadyTasksLists`, etc. still work.

> The helper reads TCB/list fields **by name** from DWARF (not hardcoded offsets), so it
> tracks the struct layout. The one hardcoded assumption is the PendSV **stack-frame
> layout** (derived from `port.c`'s `xPortPendSVHandler`); it is documented at the top of
> [`docker/freertos-tasks.py`](../docker/freertos-tasks.py). If a reconstructed backtrace
> looks wrong, check that assumption against the target first.

## flash / reset now route through the daemon (probe contention)

Only one openocd can own the CMSIS-DAP probe. So `flash.sh` and `reset.sh` no longer
spawn their own openocd — that would collide with the daemon. Instead they send the
command to the **same** running openocd via its TCL-RPC port (`scripts/ocd.sh` →
`docker/ocd-client.py`, wrapped in `capture { … }` so you see the console output):

```sh
scripts/flash.sh             # -> daemon: halt both cores; program <elf> verify reset
scripts/reset.sh             # -> daemon: reset run
scripts/ocd.sh 'targets'     # ad-hoc: any openocd/TCL command to the daemon
scripts/ocd.sh 'reg pc'      # e.g. current target's PC
```

`flash.sh`/`reset.sh`/`gdb.sh` all call `ensure_dbgd` first, so the daemon is up
before any probe traffic — **you never start openocd yourself**.

**Tradeoff / rationale:** routing through the one daemon (rather than stopping it,
spawning a one-shot openocd, and restarting it) means the probe has a single owner at
all times — no stop/start race, no contention window, and the agent never has to
think about openocd lifetime. The cost is that flash/reset depend on the daemon being
healthy; if its openocd ever wedges, `scripts/dbgd.sh restart` re-homes it (it also
auto-recovers across probe re-enumeration via its inner reconnect loop). The old
one-shot `dbg` service still exists for manual use if you ever stop the daemon
(`scripts/dbgd.sh stop` frees the probe for an external openocd).

> Flashing was **not** hardware-validated in this change (the board/probe were in use);
> the daemon, dual-core backtraces, `ocd.sh`, and resume were. The flash path uses the
> same halt-both-cores + `program … verify reset` sequence as before, just delivered
> over TCL-RPC; `flash.sh` checks the reply for `Verified OK` and fails loudly otherwise.

## Daemon lifecycle (`scripts/dbgd.sh`)

```sh
scripts/dbgd.sh start | stop | restart | rebuild | status | logs
```

You rarely need it — the daemon auto-starts whenever a script touches the probe.
`logs` follows openocd's stdout (handy to confirm `Listening on port 3333 …`).
`stop` releases the probe (e.g. to hand it to an external debugger). After a
Dockerfile change, `rebuild`.

## Gotchas

- **SWD reset ≠ physical reset** still applies (see `BUILDING.md` / the usb-stack
  saga) — routing `reset run` through the daemon doesn't change that.
- **Halting core1 pauses the USB host loop.** `gdb.sh` resumes on exit; if you use
  `--no-resume`, resume with `scripts/ocd.sh 'rp2040.core0 resume; rp2040.core1 resume'`.
- **One probe owner.** If the daemon is up, don't also run a bare `openocd`/`scripts/
  dbg` against the probe — stop the daemon first (`scripts/dbgd.sh stop`).
