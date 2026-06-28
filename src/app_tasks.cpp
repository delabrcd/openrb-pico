/*
 * Centralised FreeRTOS task creation. Each long-lived task gets a StaticTask<>
 * instance (owns its stack + TCB) and is pinned to a core here. As the superloop is
 * split into per-concern tasks (Phases 3-4), add instances below rather than
 * repeating the static-buffer boilerplate at each call site.
 */
#include "static_task.hpp"

#include "app_tasks.h"

// core affinity masks (bit N = core N). core1 runs ONLY the PIO-USB host task so its
// bit timing sees ~no context switches; every other task is pinned to core0.
static constexpr UBaseType_t kCore0Affinity = 1u << 0;
static constexpr UBaseType_t kCore1Affinity = 1u << 1;

// Priorities (higher = more urgent). The two USB stacks sit at the top; instrument
// input below; periodic housekeeping lowest. Each task blocks (OSAL queue or
// vTaskDelay), so lower-priority tasks run whenever a higher one is waiting.
static constexpr UBaseType_t kUsbTaskPriority = configMAX_PRIORITIES - 2;  // 6
static constexpr UBaseType_t kInputPriority = configMAX_PRIORITIES - 3;    // 5
static constexpr UBaseType_t kHousekeepingPriority = configMAX_PRIORITIES - 4;  // 4

static constexpr size_t kUsbHostStackWords = 2048;
static constexpr size_t kUsbDeviceStackWords = 1536;
static constexpr size_t kDrumInputStackWords = 1024;
static constexpr size_t kHousekeepingStackWords = 768;

static StaticTask<kUsbHostStackWords> s_usb_host_task;
static StaticTask<kUsbDeviceStackWords> s_usb_device_task;
static StaticTask<kDrumInputStackWords> s_drum_input_task;
static StaticTask<kHousekeepingStackWords> s_housekeeping_task;

extern "C" void app_start_tasks(void) {
    s_usb_host_task.start("usb_host", usb_host_task, nullptr, kUsbTaskPriority, kCore1Affinity);
    s_usb_device_task.start("usb_dev", usb_device_task, nullptr, kUsbTaskPriority, kCore0Affinity);
    s_drum_input_task.start("drum_in", drum_input_task, nullptr, kInputPriority, kCore0Affinity);
    s_housekeeping_task.start("housekeep", housekeeping_task, nullptr, kHousekeepingPriority,
                              kCore0Affinity);
}
