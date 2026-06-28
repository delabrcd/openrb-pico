# GDB (python) FreeRTOS SMP task-walker for scripts/gdb.sh -- sourced via `gdb -x`.
#
# WHY THIS EXISTS
# ---------------
# openocd 0.12's built-in `-rtos FreeRTOS` does NOT work for this firmware: it expects
# the single-core symbol `pxCurrentTCB`, but the SMP RP2040 port (FreeRTOS-Kernel
# V11.2.0, portable/ThirdParty/GCC/RP2040) keeps a per-core array `pxCurrentTCBs[]`.
# So `scripts/gdb.sh` only shows each core's *currently executing* stack -- not every
# task. This helper enumerates ALL FreeRTOS tasks by walking the kernel's task lists
# (in shared RAM) and prints each task's name + state + a backtrace, reconstructing the
# saved CPU context for non-running tasks from the port's PendSV stack frame layout.
#
# It connects to ONE gdbserver (RAM is shared between cores, so either reads the same
# kernel structures). scripts/gdb.sh connects this against core0 (:3333) and, because
# the other core's RUNNING task has its live registers in that core's CPU (not in RAM),
# separately dumps core1's live backtrace via docker/gdb-dump.py. See docs/DEBUGGING.md.
#
# ASSUMPTIONS (verify against the target -- see the report / docs/DEBUGGING.md):
#
#   * TCB layout: we read fields BY NAME via gdb's debug info (`tcb['pxTopOfStack']`,
#     ['pcTaskName'], ['uxPriority'], ['xTaskRunState'], ['uxTaskAttributes'],
#     ['xStateListItem']). So we do NOT hardcode struct offsets -- gdb resolves them
#     from DWARF. uxTCBNumber is only present when configUSE_TRACE_FACILITY==1 (it is 0
#     here), so it is printed only if the field exists.
#
#   * Task-list globals (static in tasks.c, resolved by name from DWARF):
#       pxReadyTasksLists[configMAX_PRIORITIES]  (configMAX_PRIORITIES == 8)
#       xDelayedTaskList1 / xDelayedTaskList2     (the two delayed lists)
#       xPendingReadyList, xSuspendedTaskList, xTasksWaitingTermination
#       pxCurrentTCBs[configNUMBER_OF_CORES]      (configNUMBER_OF_CORES == 2)
#       uxCurrentNumberOfTasks
#     List_t walk uses xListEnd (end marker), pxNext, pvOwner (-> TCB).
#
#   * Saved-context stack frame (Cortex-M0+, from xPortPendSVHandler in port.c, SMP
#     branch). On a context switch the port does, for the outgoing task:
#         subs r1, r1, #32        ; psp -= 32  (room for 8 sw-saved low regs)
#         str  r1, [r0]           ; pxTopOfStack = psp
#         stmia r1!, {r4-r7}      ; [sp+0..12]  = r4,r5,r6,r7
#         mov  r4..r7, r8..r11 ; stmia r1!, {r4-r7}  ; [sp+16..28] = r8,r9,r10,r11
#     Above that sits the hardware-stacked exception frame (r0-r3,r12,lr,pc,xpsr).
#     So, relative to pxTopOfStack (SP):
#         SP+0..28  : r4 r5 r6 r7 r8 r9 r10 r11   (software-saved low frame)
#         SP+32     : r0
#         SP+36     : r1
#         SP+40     : r2
#         SP+44     : r3
#         SP+48     : r12
#         SP+52     : lr   (r14)
#         SP+56     : pc   (return address)
#         SP+60     : xpsr
#     The task's SP at the point of preemption (what we set $sp to for unwinding) is
#     SP+64, plus 4 if xpsr bit 9 (0x200) is set (the hardware 8-byte-alignment pad).
#     NOTE: portUSE_DIVIDER_SAVE_RESTORE stores the SIO divider state in 4 words BELOW
#     pxTopOfStack (the port does `subs r0,r0,#48` AFTER recording the SP, precisely so
#     it does not disturb the frame a debugger sees), so it does NOT shift these
#     offsets. Cortex-M0+ has no FPU, so there is no FP context to skip.
import os

host = os.environ.get("GDB_HOST", "dbgd")
port = os.environ.get("GDB_PORT", "3333")
resume = os.environ.get("GDB_RESUME", "1") == "1"
# Which core's gdbserver we are connected to (3333 -> 0, 3334 -> 1). The task running
# on THIS core has its live registers in this core's CPU, so we bt it live; the task
# running on the OTHER core lives in that core's CPU (not RAM), so we cannot read its
# live registers here -- we flag it and point at `scripts/gdb.sh coreN bt`.
connected_core = int(os.environ.get("GDB_CORE", "0"))

NUM_CORES = 2
MAX_PRIORITIES = 8
taskATTRIBUTE_IS_IDLE = 0x1

gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("set print pretty off")


def read_u32(addr):
    mem = gdb.selected_inferior().read_memory(addr, 4)
    return int.from_bytes(bytes(mem), "little")


def eval_global(name):
    """Resolve a (possibly file-static) kernel global; return gdb.Value or None."""
    for expr in (name, "'tasks.c'::%s" % name):
        try:
            return gdb.parse_and_eval(expr)
        except gdb.error:
            continue
    return None


def tcb_ptr_type():
    return gdb.lookup_type("TCB_t").pointer()


def tcb_field(tcb, name):
    try:
        return tcb[name]
    except (gdb.error, KeyError):
        return None


def task_name(tcb):
    try:
        s = tcb["pcTaskName"]
        # char[configMAX_TASK_NAME_LEN]; string() stops at NUL.
        return s.string(errors="replace")
    except (gdb.error, KeyError):
        return "??"


def walk_list(list_val):
    """Yield TCB_t* (gdb.Value) for every item in a List_t, via pvOwner."""
    if list_val is None:
        return
    try:
        count = int(list_val["uxNumberOfItems"])
    except (gdb.error, KeyError):
        return
    if count <= 0:
        return
    end = list_val["xListEnd"]            # MiniListItem_t (the end marker)
    end_addr = int(end.address)
    node = end["pxNext"]                  # ListItem_t*
    tptr = tcb_ptr_type()
    seen = 0
    # Guard against a corrupt/looping list: never iterate more than count items.
    while int(node) != end_addr and seen < count:
        item = node.dereference()
        owner = item["pvOwner"]
        if int(owner) != 0:
            yield owner.cast(tptr)
        node = item["pxNext"]
        seen += 1


def reconstruct_frame(sp):
    """Return dict of recovered registers for a non-running task from pxTopOfStack."""
    regs = {}
    # software-saved low frame: r4..r11 at SP+0..28
    for i, name in enumerate(("r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11")):
        regs[name] = read_u32(sp + i * 4)
    # hardware exception frame: r0-r3,r12,lr,pc,xpsr at SP+32..60
    regs["r0"] = read_u32(sp + 32)
    regs["r1"] = read_u32(sp + 36)
    regs["r2"] = read_u32(sp + 40)
    regs["r3"] = read_u32(sp + 44)
    regs["r12"] = read_u32(sp + 48)
    regs["lr"] = read_u32(sp + 52)
    regs["pc"] = read_u32(sp + 56)
    xpsr = read_u32(sp + 60)
    regs["xpsr"] = xpsr
    frame_sp = sp + 64
    if xpsr & 0x200:        # hardware 8-byte stack-alignment padding
        frame_sp += 4
    regs["sp"] = frame_sp
    return regs


# Registers we save/restore around a non-running-task backtrace. Setting these on the
# halted core temporarily rewrites the live CPU register cache; we ALWAYS restore them
# before resuming so the connected core resumes exactly as it was.
SAVE_REGS = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
             "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"]


def snapshot_regs():
    snap = {}
    for r in SAVE_REGS:
        try:
            snap[r] = int(gdb.parse_and_eval("$%s" % r)) & 0xFFFFFFFF
        except gdb.error:
            snap[r] = None
    return snap


def apply_regs(regs):
    # Order: low/high regs first, then sp, lr, pc last.
    for r in SAVE_REGS:
        if r in regs and regs[r] is not None:
            try:
                gdb.execute("set $%s = 0x%x" % (r, regs[r] & 0xFFFFFFFF))
            except gdb.error:
                pass


def backtrace_str():
    try:
        return gdb.execute("bt", to_string=True).rstrip()
    except gdb.error as e:
        return "[bt error] %s" % e


def collect_tasks():
    """Return (ordered list of (tcb_addr, tcb_value, state_label), running_map)."""
    tasks = {}        # addr -> [tcb_value, state_label]
    order = []        # preserve discovery order

    def add(tcb, label):
        addr = int(tcb)
        if addr == 0:
            return
        if addr not in tasks:
            tasks[addr] = [tcb, label]
            order.append(addr)
        # don't overwrite an existing label (ready/blocked/suspended found first wins)

    # Ready lists (per priority).
    ready = eval_global("pxReadyTasksLists")
    if ready is not None:
        for prio in range(MAX_PRIORITIES):
            try:
                lst = ready[prio]
            except gdb.error:
                continue
            for tcb in walk_list(lst):
                add(tcb, "READY(prio %d)" % prio)

    # Delayed lists (two of them: current + overflow).
    for nm in ("xDelayedTaskList1", "xDelayedTaskList2"):
        for tcb in walk_list(eval_global(nm)):
            add(tcb, "BLOCKED(delayed)")

    # Pending-ready (readied while scheduler suspended).
    for tcb in walk_list(eval_global("xPendingReadyList")):
        add(tcb, "READY(pending)")

    # Suspended.
    for tcb in walk_list(eval_global("xSuspendedTaskList")):
        add(tcb, "SUSPENDED")

    # Waiting termination (deleted, memory not yet freed).
    for tcb in walk_list(eval_global("xTasksWaitingTermination")):
        add(tcb, "DELETED(waiting termination)")

    # Currently-running tasks per core (mark/override their state).
    running = {}      # core -> tcb_addr
    cur = eval_global("pxCurrentTCBs")
    if cur is not None:
        for core in range(NUM_CORES):
            try:
                tcb = cur[core]
            except gdb.error:
                continue
            if int(tcb) == 0:
                continue
            running[core] = int(tcb)
            add(tcb, "RUNNING(core %d)" % core)
            tasks[int(tcb)][1] = "RUNNING(core %d)" % core

    return order, tasks, running


def run():
    print("\n=========================  FreeRTOS SMP task walk  "
          "=========================")
    n = eval_global("uxCurrentNumberOfTasks")
    if n is not None:
        print("uxCurrentNumberOfTasks = %d   (connected to core%d's gdbserver)"
              % (int(n), connected_core))

    try:
        tcb_ptr_type()
    except gdb.error:
        print("!! TCB_t type not found in symbols -- is this the matching ELF? "
              "(scripts/build.sh, then point ORB_BUILD_DIR/ELF at it)")
        return

    order, tasks, running = collect_tasks()
    if not order:
        print("!! no tasks found -- scheduler not started yet, or list symbols "
              "unresolved (try `p pxReadyTasksLists` by hand).")
        return

    other_core_running = running.get(1 - connected_core)

    # Snapshot the connected core's live registers ONCE; restore at the very end.
    saved = snapshot_regs()
    try:
        for addr in order:
            tcb, label = tasks[addr]
            name = task_name(tcb)
            try:
                prio = int(tcb["uxPriority"])
            except (gdb.error, KeyError):
                prio = -1
            extra = ""
            tcbnum = tcb_field(tcb, "uxTCBNumber")
            if tcbnum is not None:
                extra += "  tcb#%d" % int(tcbnum)
            attrs = tcb_field(tcb, "uxTaskAttributes")
            if attrs is not None and (int(attrs) & taskATTRIBUTE_IS_IDLE):
                extra += "  [idle]"

            print("\n--------------------------------------------------------------"
                  "------------------")
            print("task '%s'  prio=%d  state=%s%s  TCB=0x%08x"
                  % (name, prio, label, extra, addr))

            if running.get(connected_core) == addr:
                # The connected core's running task: its live registers are correct.
                print("  (running on this core -- LIVE backtrace)")
                # Make sure we're on the live registers (restore in case a previous
                # iteration left reconstructed regs in place).
                apply_regs(saved)
                print(backtrace_str())
                continue

            if other_core_running == addr:
                # Running on the OTHER core: live registers are in that CPU, not RAM.
                # Reconstructing from pxTopOfStack would show the LAST context-switch-out
                # frame (often just the initial entry frame for a core1-pinned task that
                # never gets preempted), which is misleading -- so we don't.
                print("  (running on core%d -- live registers are in that core's CPU, "
                      "not\n   reachable from this gdbserver. For its live backtrace: "
                      "scripts/gdb.sh core%d bt)" % (1 - connected_core, 1 - connected_core))
                continue

            # Non-running task: reconstruct its saved context and unwind.
            try:
                sp = int(tcb["pxTopOfStack"])
            except (gdb.error, KeyError):
                print("  [cannot read pxTopOfStack]")
                continue
            try:
                regs = reconstruct_frame(sp)
            except gdb.MemoryError as e:
                print("  [cannot read saved frame at 0x%08x: %s]" % (sp, e))
                continue
            print("  (reconstructed from pxTopOfStack=0x%08x -> pc=0x%08x lr=0x%08x "
                  "sp=0x%08x)" % (sp, regs["pc"], regs["lr"], regs["sp"]))
            apply_regs(regs)
            print(backtrace_str())
            apply_regs(saved)     # restore live regs after each reconstruction
    finally:
        apply_regs(saved)         # belt-and-suspenders: never leave fake regs live

    if other_core_running is not None:
        print("\n>> NOTE: the task running on core%d is shown above without a live "
              "backtrace.\n   Run `scripts/gdb.sh core%d bt` for it (scripts/gdb.sh "
              "tasks does this for you)." % (1 - connected_core, 1 - connected_core))


gdb.execute("set print pretty on")
try:
    gdb.execute("target extended-remote %s:%s" % (host, port))
except gdb.error as e:
    print("!! could not connect to %s:%s -- is the dbgd daemon up? "
          "(scripts/dbgd.sh status)  [%s]" % (host, port, e))
    gdb.execute("quit 1")

try:
    try:
        gdb.execute("monitor halt")
    except gdb.error as e:
        print("[warn] halt: %s" % e)
    run()
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
