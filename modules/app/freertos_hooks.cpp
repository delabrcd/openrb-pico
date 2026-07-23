/*
 * FreeRTOS application hooks for openrb-pico.
 *
 * These are required by the config in inc/FreeRTOSConfig.h:
 *   - configUSE_MALLOC_FAILED_HOOK  -> vApplicationMallocFailedHook
 *   - configCHECK_FOR_STACK_OVERFLOW -> vApplicationStackOverflowHook
 *
 * Both are dev-time backstops: heap_4 exhaustion or a task stack overflow should
 * never happen in normal operation. They log (deferred, core-safe) and hang so a
 * watchdog/debugger catches it rather than silently corrupting memory. For a ship
 * build, set those two config options to 0 and drop this file from the build.
 *
 * The FreeRTOS kernel calls these by C symbol, so they are the vendor/RTOS seam and
 * stay extern "C" free functions (docs/architecture.md principle 5).
 */

#include "FreeRTOS.h"
#include "task.h"

#include "dlog.h"

extern "C" {

void vApplicationMallocFailedHook(void) {
    dlog_printf("FATAL: FreeRTOS pvPortMalloc failed (heap exhausted)\r\n");
    portDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName) {
    (void)xTask;
    dlog_printf("FATAL: FreeRTOS stack overflow in task '%s'\r\n",
                pcTaskName ? pcTaskName : "?");
    portDISABLE_INTERRUPTS();
    for (;;) {
    }
}

}  // extern "C"
