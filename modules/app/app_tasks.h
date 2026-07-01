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

// Task entry points (defined in modules/app/main.cpp). core1 hosts ONLY usb_host_task; every
// other task is pinned to core0.
void usb_host_task(void *param);
void usb_device_task(void *param);  // device stack + send drain
void drum_input_task(void *param);  // USB-MIDI + serial instrument input
void instrument_task(void *param);  // sole applier of instrument connect/disconnect events
void housekeeping_task(void *param);  // announce + recovery + log drain

// Create all application tasks with their stacks/affinities. Call once from main()
// before vTaskStartScheduler().
void app_start_tasks(void);

#ifdef __cplusplus
}
#endif

