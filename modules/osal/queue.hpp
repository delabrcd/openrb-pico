/*
 * osal (OS Abstraction Layer): this header is part of the firmware's ONLY dependency on
 * FreeRTOS. inc/osal/ is the portability boundary — only this osal layer (and the hal layer) may
 * include FreeRTOS/pico-sdk; the rest of the codebase uses orb::osal::* and never touches
 * the RTOS directly. Porting to another RTOS means rewriting inc/osal/, nothing else.
 *
 * orb::osal::Queue<T, Depth> — a tiny RAII wrapper that owns a FreeRTOS queue's storage +
 * control block and creates it statically (no heap), the sibling of orb::osal::Task<N>
 * (inc/osal/task.hpp). It removes the "declare a uint8_t storage[] + a StaticQueue_t +
 * call xQueueCreateStatic + keep a QueueHandle_t" boilerplate that app_queues repeated
 * per queue (see docs/architecture.md D1).
 *
 * Same discipline as Task: storage members are the FreeRTOS Static*_t buffers,
 * construction does nothing kernel-touching (trivial type, lands in BSS, no global-ctor
 * / static-init-order concern), and create() does the actual xQueueCreateStatic once
 * the kernel is up enough. Give each instance static storage duration.
 *
 *   static orb::osal::Queue<XboxPacket, 8> host_tx;
 *   host_tx.create();
 *   if (!host_tx.send(pkt)) { ... full ... }   // non-blocking, matches app_queues
 *
 * send()/recv() are non-blocking (timeout 0) to match the existing app_queues semantics
 * exactly; add explicit timed variants only if a caller needs to block.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "FreeRTOS.h"
#include "queue.h"

namespace orb::osal {

template <typename T, size_t Depth>
class Queue {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Queue<T>: T must be trivially copyable (FreeRTOS moves items by memcpy)");

   public:
    // Create the queue. Call once, before/while the scheduler runs. Never null for a
    // static create.
    QueueHandle_t create() {
        handle_ = xQueueCreateStatic(Depth, sizeof(T), storage_, &ctrl_);
        return handle_;
    }

    // Non-blocking enqueue/dequeue (timeout 0). Return false if full / empty.
    bool send(const T& item) { return xQueueSend(handle_, &item, 0) == pdTRUE; }
    bool recv(T& out) { return xQueueReceive(handle_, &out, 0) == pdTRUE; }

    // Blocking dequeue: park the calling task until an item arrives. For a dedicated
    // consumer task that has nothing else to do (e.g. the instrument owner task) — it
    // costs zero CPU while idle and wakes immediately on send(). portMAX_DELAY stays
    // inside osal so callers never name a FreeRTOS symbol (portability boundary).
    bool recv_blocking(T& out) { return xQueueReceive(handle_, &out, portMAX_DELAY) == pdTRUE; }

    QueueHandle_t handle() const { return handle_; }

   private:
    uint8_t storage_[Depth * sizeof(T)];  // FreeRTOS item storage
    StaticQueue_t ctrl_;                   // queue control block
    QueueHandle_t handle_ = nullptr;
};

}  // namespace orb::osal

