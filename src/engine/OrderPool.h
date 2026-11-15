#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include "Order.h"

/// @brief Intrusive LIFO memory pool for zero-allocation order management.
/// @details Pre-allocates all order nodes at startup. Utilizes the intrusive `next_order` 
/// field to maintain the free list without requiring external tracking structures.
/// @tparam MaxOrders The fixed maximum capacity of the pool.
template<size_t MaxOrders>
class OrderPool {
private:
    std::array<Order, MaxOrders> orders_; /// < Pre-allocated contiguous block of orders.
    uint32_t free_head_{0};               /// < Index pointing to the head of the free list.

public:
    /// @brief Initializes the intrusive free list by chaining every slot to the next.
    OrderPool() {
        for (uint32_t i = 0; i < MaxOrders - 1; ++i) {
            orders_[i].next_order = i + 1;
        }
        orders_[MaxOrders - 1].next_order = NULL_INDEX;
    }

    /// @brief O(1) capacity check for Pre-Trade Validation.
    /// @return True if the pool has exhausted its capacity.
    bool is_full() const {
        return free_head_ == NULL_INDEX;
    }

    /// @brief Allocates an order from the pool.
    /// @return The index of the allocated order, or NULL_INDEX if full.
    uint32_t allocate() {
        if (free_head_ == NULL_INDEX) [[unlikely]] {
            return NULL_INDEX;
        }
        uint32_t index = free_head_;
        free_head_ = orders_[index].next_order; 
        return index;
    }

    /// @brief Returns an order to the free pool.
    /// @param index The pool index of the order to deallocate.
    void deallocate(uint32_t index) {
        orders_[index].next_order = free_head_;
        free_head_ = index;
    }

    /// @brief Retrieves a mutable reference to an order by its index.
    /// @param index The pool index.
    /// @return Reference to the order.
    Order& get(uint32_t index) {
        return orders_[index];
    }

    /// @brief Retrieves a constant reference to an order by its index.
    /// @param index The pool index.
    /// @return Constant reference to the order.
    const Order& get(uint32_t index) const {
        return orders_[index];
    }
};