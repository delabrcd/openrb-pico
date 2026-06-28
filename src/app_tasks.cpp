/*
 * Centralised FreeRTOS task creation. Each long-lived task gets a StaticTask<>
 * instance (owns its stack + TCB) and is pinned to a core here. As the superloop is
 * split into per-concern tasks (Phases 3-4), add instances below rather than
 * repeating the static-buffer boilerplate at each call site.
 */
#include "static_task.hpp"

#include "app_tasks.h"

// core affinity masks (bit N = core N). core1 runs ONLY the PIO-USB host task so its
// bit timing sees ~no context switches; everything else is pinned to core0.
static constexpr UBaseType_t kCore0Affinity = 1u << 0;
static constexpr UBaseType_t kCore1Affinity = 1u << 1;

// USB stacks run at the highest application priority; feature tasks (added later)
// sit below them.
static constexpr UBaseType_t kUsbTaskPriority = configMAX_PRIORITIES - 2;

static constexpr size_t kUsbHostStackWords = 2048;
static constexpr size_t kCore0StackWords = 2048;

static StaticTask<kUsbHostStackWords> s_usb_host_task;
static StaticTask<kCore0StackWords> s_core0_task;

extern "C" void app_start_tasks(void) {
    s_usb_host_task.start("usb_host", usb_host_task, nullptr, kUsbTaskPriority, kCore1Affinity);
    s_core0_task.start("core0", core0_task, nullptr, kUsbTaskPriority, kCore0Affinity);
}
