/*
 * Task bootstrap for the FreeRTOS SMP port. The task *bodies* live in their feature
 * modules; the *creation* (stack/TCB storage + affinity) is centralised in
 * app_tasks.cpp via the orb::osal::Task<> template. main() calls app_start_tasks() then
 * vTaskStartScheduler().
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Create all application tasks with their stacks/affinities. Call once from main()
// before vTaskStartScheduler().
void app_start_tasks(void);

#ifdef __cplusplus
}
#endif

