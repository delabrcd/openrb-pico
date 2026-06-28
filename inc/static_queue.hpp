/*
 * StaticQueue<T, Depth> — a tiny RAII wrapper that owns a FreeRTOS queue's storage +
 * control block and creates it statically (no heap), the sibling of StaticTask<N>
 * (inc/static_task.hpp). It removes the "declare a uint8_t storage[] + a StaticQueue_t +
 * call xQueueCreateStatic + keep a QueueHandle_t" boilerplate that app_queues repeated
 * per queue (see docs/features/cpp-overhaul.md D1).
 *
 * Same discipline as StaticTask: storage members are the FreeRTOS Static*_t buffers,
 * construction does nothing kernel-touching (trivial type, lands in BSS, no global-ctor
 * / static-init-order concern), and create() does the actual xQueueCreateStatic once
 * the kernel is up enough. Give each instance static storage duration.
 *
 *   static StaticQueue<xbox_packet_t, 8> host_tx;
 *   host_tx.create();
 *   if (!host_tx.send(pkt)) { ... full ... }   // non-blocking, matches app_queues
 *
 * send()/recv() are non-blocking (timeout 0) to match the existing app_queues semantics
 * exactly; add explicit timed variants only if a caller needs to block.
 */
#ifndef OPENRB_STATIC_QUEUE_HPP
#define OPENRB_STATIC_QUEUE_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "FreeRTOS.h"
#include "queue.h"

namespace orb {

template <typename T, size_t Depth>
class StaticQueue {
    static_assert(std::is_trivially_copyable<T>::value,
                  "StaticQueue<T>: T must be trivially copyable (FreeRTOS moves items by memcpy)");

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

    QueueHandle_t handle() const { return handle_; }

   private:
    uint8_t storage_[Depth * sizeof(T)];  // FreeRTOS item storage
    StaticQueue_t ctrl_;                   // queue control block
    QueueHandle_t handle_ = nullptr;
};

}  // namespace orb

#endif  // OPENRB_STATIC_QUEUE_HPP
