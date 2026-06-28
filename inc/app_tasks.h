/*
 * Task bootstrap for the FreeRTOS SMP port. The task *bodies* live in their feature
 * modules (C); the *creation* (stack/TCB storage + affinity) is centralised in
 * app_tasks.cpp via the StaticTask<> template. main() calls app_start_tasks() then
 * vTaskStartScheduler().
 */
#ifndef OPENRB_APP_TASKS_H
#define OPENRB_APP_TASKS_H

#ifdef __cplusplus
extern "C" {
#endif

// Task entry points (defined in src/main.c). core1 hosts ONLY usb_host_task; every
// other task is pinned to core0.
void usb_host_task(void *param);
void usb_device_task(void *param);  // device stack + send drain
void drum_input_task(void *param);  // USB-MIDI + serial instrument input
void housekeeping_task(void *param);  // announce + recovery + log drain

// Create all application tasks with their stacks/affinities. Call once from main()
// before vTaskStartScheduler().
void app_start_tasks(void);

#ifdef __cplusplus
}
#endif

#endif  // OPENRB_APP_TASKS_H
