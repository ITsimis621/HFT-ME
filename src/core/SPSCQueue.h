#pragma once

#include <atomic>
#include <array>
#include <cstddef>

/// @brief Standard L1 cache line size to prevent false sharing between threads.
constexpr size_t CACHE_LINE_SIZE = 64;

/// @brief A wait-free, bounded Single-Producer Single-Consumer (SPSC) ring buffer.
/// @details Designed for ultra-low-latency thread hand-offs using cached cursors 
/// to minimize atomic memory contention across the CPU interconnect.
/// @tparam T The payload type stored in the queue.
/// @tparam Capacity The maximum capacity of the queue (must be a power of 2).
template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0), 
                  "SPSCQueue capacity must be a power of 2 for bitwise masking");
private:
    static constexpr size_t MASK = Capacity - 1;

    /// @brief Producer state padded to a full cache line.
    /// @details Exclusively written by the Producer thread to avoid false sharing.
    struct alignas(CACHE_LINE_SIZE) ProducerState {
        std::atomic<size_t> tail{0}; /// < Atomic tail cursor visible to the consumer.
        size_t cached_head{0};       /// < Local cached head to minimize atomic read contention.
    } producer_;

    /// @brief Consumer state padded to a full cache line.
    /// @details Exclusively written by the Consumer thread to avoid false sharing.
    struct alignas(CACHE_LINE_SIZE) ConsumerState {
        std::atomic<size_t> head{0}; /// < Atomic head cursor visible to the producer.
        size_t cached_tail{0};       /// < Local cached tail to minimize atomic read contention.
    } consumer_;

    /// @brief The payload buffer, forced to its own cache line so data modifications do not invalidate cursor cache lines.
    alignas(CACHE_LINE_SIZE) std::array<T, Capacity> buffer_;

public:
    SPSCQueue() = default;

    /// @brief Fast-path capacity check for the producer thread.
    /// @details Evaluates available space without executing a heavy atomic load on the consumer's head 
    /// unless the local cached head indicates insufficient capacity.
    /// @param required_slots The number of slots to verify.
    /// @return True if enough capacity is available, otherwise false.
    bool has_capacity(size_t required_slots = 1) noexcept {
        if (required_slots >= Capacity) [[unlikely]] {
            return false;
        }

        const size_t current_tail = producer_.tail.load(std::memory_order_relaxed);
        size_t used = (current_tail - producer_.cached_head) & MASK;
        size_t free_slots = MASK - used;

        if (free_slots < required_slots) {
            producer_.cached_head = consumer_.head.load(std::memory_order_acquire);
            used = (current_tail - producer_.cached_head) & MASK;
            free_slots = MASK - used;
        }

        return free_slots >= required_slots;
    }

    /// @brief Attempts to enqueue an item into the ring buffer.
    /// @param item The payload to push.
    /// @return True on success, false if the queue is full.
    bool push(const T& item) noexcept {
        const size_t current_tail = producer_.tail.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & MASK;

        if (next_tail == producer_.cached_head) {
            producer_.cached_head = consumer_.head.load(std::memory_order_acquire);
            if (next_tail == producer_.cached_head) {
                return false; 
            }
        }

        buffer_[current_tail] = item;
        producer_.tail.store(next_tail, std::memory_order_release);
        return true;
    }

    /// @brief Attempts to dequeue an item from the ring buffer.
    /// @param item Reference to populate with the dequeued payload.
    /// @return True on success, false if the queue is empty.
    bool pop(T& item) noexcept {
        const size_t current_head = consumer_.head.load(std::memory_order_relaxed);

        if (current_head == consumer_.cached_tail) {
            consumer_.cached_tail = producer_.tail.load(std::memory_order_acquire);
            if (current_head == consumer_.cached_tail) {
                return false; 
            }
        }

        item = buffer_[current_head];
        consumer_.head.store((current_head + 1) & MASK, std::memory_order_release);
        return true;
    }

    /// @brief Evaluates if the queue is entirely empty via strict atomic comparison.
    /// @return True if empty, otherwise false.
    bool empty() const noexcept {
        return consumer_.head.load(std::memory_order_acquire) ==
               producer_.tail.load(std::memory_order_acquire);
    }
};