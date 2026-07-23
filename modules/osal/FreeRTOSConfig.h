/*
 * FreeRTOSConfig.h for openrb-pico — RP2040 SMP port (FreeRTOS-Kernel V11.2.0,
 * portable/ThirdParty/GCC/RP2040).
 *
 * Topology (see docs/architecture.md / the FreeRTOS-SMP port plan):
 *   - SMP, both cores under the scheduler.
 *   - The PIO-USB host task is pinned to core1 and is the ONLY task allowed to run
 *     there, so core1 sees ~no context switches and PIO-USB bit timing is undisturbed.
 *   - Every other task is pinned to core0.
 *
 * The chip runs at 120 MHz (NOT overclocked); configCPU_CLOCK_HZ tracks
 * set_sys_clock_khz(120000) in modules/app/main.cpp.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*-----------------------------------------------------------
 * SMP / multicore
 *----------------------------------------------------------*/
#define configNUMBER_OF_CORES                   2
#define configRUN_MULTIPLE_PRIORITIES           1   /* allow core0/core1 to run different priorities */
#define configUSE_CORE_AFFINITY                 1   /* vTaskCoreAffinitySet: host->core1, rest->core0 */
#define configUSE_PASSIVE_IDLE_HOOK             0   /* must be defined under SMP */
#define configTICK_CORE                         0   /* SysTick owned by core0 (keep authoritative tick off core1) */
/* configSMP_SPINLOCK_0/1 default to PICO_SPINLOCK_ID_OS1/OS2 (unused by our code). */

/*-----------------------------------------------------------
 * Scheduler
 *----------------------------------------------------------*/
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0   /* required 0 on Cortex-M0+ */
#define configCPU_CLOCK_HZ                      120000000   /* must match set_sys_clock_khz(120000) */
#define configTICK_RATE_HZ                      1000
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                256         /* words */
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configSTACK_DEPTH_TYPE                  uint32_t

/*-----------------------------------------------------------
 * Memory allocation
 *----------------------------------------------------------*/
#define configSUPPORT_STATIC_ALLOCATION         1   /* long-lived tasks use xTaskCreateStatic */
#define configKERNEL_PROVIDED_STATIC_MEMORY     1   /* kernel supplies idle/timer/passive-idle task memory */
#define configSUPPORT_DYNAMIC_ALLOCATION        1   /* heap_4 still linked (malloc, any dynamic objects) */
#define configTOTAL_HEAP_SIZE                   (64 * 1024)
/* heap_4 placed in the default region; we do not override the malloc region. */

/*-----------------------------------------------------------
 * Synchronization primitives (TinyUSB OSAL needs mutex/sem/queue/notify;
 * our code uses software timers and task notifications)
 *----------------------------------------------------------*/
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   2
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               8

/*-----------------------------------------------------------
 * Software timers (replaces the midi.c hardware_alarm disconnect timeout)
 *----------------------------------------------------------*/
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            256
// Pin the timer-service daemon to core0. It is higher priority than usb_host_task, so
// unpinned (tskNO_AFFINITY) the SMP scheduler could run it on core1 and preempt the
// bit-banged PIO-USB host loop. The midi disconnect software timer is the only timer
// user; its callback (disconnect_instrument) belongs on core0 with the feature logic.
#define configTIMER_SERVICE_TASK_CORE_AFFINITY  (1u << 0)

/*-----------------------------------------------------------
 * Hooks / dev instrumentation (drop CHECK_FOR_STACK_OVERFLOW + MALLOC hook for ship)
 *----------------------------------------------------------*/
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configRECORD_STACK_HIGH_ADDRESS         1

/*-----------------------------------------------------------
 * Runtime stats / co-routines
 *----------------------------------------------------------*/
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/*-----------------------------------------------------------
 * Optional API
 *----------------------------------------------------------*/
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1   /* required by the SMP port */
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xQueueGetMutexHolder            1

/*-----------------------------------------------------------
 * Assertion handler (Cortex-M0+ has no BASEPRI, so there is no
 * configMAX_SYSCALL_INTERRUPT_PRIORITY; FreeRTOS critical sections mask all IRQs).
 * Use portDISABLE_INTERRUPTS() (not taskDISABLE_INTERRUPTS()): configASSERT is
 * expanded inside portmacro.h's vPortRecursiveLock, before task.h defines the
 * task-level macro — so only port-level macros are safe to call here.
 *----------------------------------------------------------*/
#define configASSERT(x)                                       \
    if ((x) == 0) {                                           \
        portDISABLE_INTERRUPTS();                             \
        for (;;) {                                            \
        }                                                    \
    }

#endif /* FREERTOS_CONFIG_H */
