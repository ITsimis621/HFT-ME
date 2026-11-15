#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <nmmintrin.h> // For SSE4.2 _mm_crc32_u64

/// @brief A high-performance, open-addressed hash map optimized for zero-allocation environments.
/// @details Uses linear probing and a backward-shift deletion algorithm to avoid tombstones.
/// Hardware-accelerated hashing (`_mm_crc32_u64`) ensures ultra-low latency lookups.
/// @tparam Capacity Maximum number of elements. Must be a power of 2 for fast bitwise modulo.
template<size_t Capacity>
class FlatHashMap {
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity > 0, 
                  "Capacity must be a power of 2 for bitwise index masking.");

public:
    /// @brief Sentinel value indicating an invalid or empty pool index.
    static constexpr uint32_t NULL_INDEX = 0xFFFFFFFF;

private:
    /// @brief 16-byte aligned bucket to ensure efficient cache line utilization.
    struct alignas(16) Entry {
        uint64_t key{0};            /// < The Order ID (0 indicates an empty bucket).
        uint32_t value{NULL_INDEX}; /// < The index in the OrderPool.
        uint32_t padding{0};        /// < Explicit padding for 16-byte alignment.
    };

    std::array<Entry, Capacity> table_;
    size_t size_{0};           

    /// @brief Computes the hash of an Order ID using hardware SSE4.2 instructions.
    /// @details Combines CRC32 with a 64-bit golden ratio multiplier to achieve 
    /// a strong avalanche effect, minimizing clustering in the linear probing chain.
    /// @param key The Order ID to hash.
    /// @return The computed hash value (unmasked).
    inline size_t hash(uint64_t key) const {
        uint64_t crc = _mm_crc32_u64(0, key);
        return static_cast<size_t>(crc * 0x9E3779B97F4A7C15ULL);
    }

public:
    FlatHashMap() = default;

    /// @brief Returns the current number of active entries.
    size_t size() const { return size_; }

    /// @brief O(1) capacity check used during pre-trade validation.
    /// @details Hard-capped at 50% load factor to guarantee short linear probing chains.
    /// @return True if the map has reached its maximum efficient capacity.
    bool is_full() const {
        return size_ >= (Capacity / 2);
    }

    /// @brief Inserts a new Order ID and its corresponding pool index into the map.
    /// @param order_id The unique identifier for the order.
    /// @param pool_index The physical index in the OrderPool.
    /// @return True on successful insertion, false if full or duplicate ID detected.
    bool insert(uint64_t order_id, uint32_t pool_index) {
        if (order_id == 0 || pool_index == NULL_INDEX) [[unlikely]] return false;
        if (is_full()) [[unlikely]] return false; 

        size_t idx = hash(order_id) & (Capacity - 1);

        while (table_[idx].key != 0) {
            if (table_[idx].key == order_id) [[unlikely]] {
                return false; // Reject duplicate order_id
            }
            idx = (idx + 1) & (Capacity - 1);
        }

        table_[idx] = {order_id, pool_index, 0};
        size_++;
        return true;
    }

    /// @brief Retrieves the pool index associated with a given Order ID.
    /// @param order_id The order to look up.
    /// @return The pool index, or NULL_INDEX if not found.
    uint32_t get(uint64_t order_id) const {
        if (order_id == 0) [[unlikely]] return NULL_INDEX;

        size_t idx = hash(order_id) & (Capacity - 1);

        while (table_[idx].key != 0) {
            if (table_[idx].key == order_id) {
                return table_[idx].value;
            }
            idx = (idx + 1) & (Capacity - 1);
        }
        return NULL_INDEX; 
    }

    /// @brief Removes an Order ID from the map and repacks the probing chain.
    /// @details Implements backward-shift deletion. By shifting subsequent valid entries backward 
    /// to fill the gap, it eliminates the need for "tombstone" markers, preventing search degradation.
    /// @param order_id The order to remove.
    void remove(uint64_t order_id) {
        if (order_id == 0) [[unlikely]] return;

        size_t idx = hash(order_id) & (Capacity - 1);

        while (table_[idx].key != 0) {
            if (table_[idx].key == order_id) {
                break;
            }
            idx = (idx + 1) & (Capacity - 1);
        }

        if (table_[idx].key != order_id) return;

        table_[idx].key = 0;
        table_[idx].value = NULL_INDEX;
        size_--;

        size_t gap_idx = idx;
        size_t next_idx = (idx + 1) & (Capacity - 1);

        while (table_[next_idx].key != 0) {
            size_t ideal_idx = hash(table_[next_idx].key) & (Capacity - 1);

            size_t dist_to_next = (next_idx - ideal_idx) & (Capacity - 1);
            size_t dist_to_gap  = (gap_idx - ideal_idx) & (Capacity - 1);

            if (dist_to_gap < dist_to_next) {
                table_[gap_idx] = table_[next_idx];
                table_[next_idx].key = 0;
                table_[next_idx].value = NULL_INDEX;
                gap_idx = next_idx;
            }
            next_idx = (next_idx + 1) & (Capacity - 1);
        }
    }
};