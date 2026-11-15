#pragma once
#include <cstdint>
#include "Order.h"

/// @brief Represents a single price tick in the Limit Order Book.
/// @details Aligned to 16 bytes to ensure optimal cache locality when iterating through the book.
struct alignas(16) PriceLevel {
    /// @brief Aggregate resting volume at this price level. 
    /// @details Sized as a 64-bit integer to prevent overflow during extreme liquidity events.
    uint64_t total_volume{0};
    
    /// @brief Index pointing to the first order in the OrderPool for this price level.
    uint32_t head_order{NULL_INDEX};
    
    /// @brief Index pointing to the last order in the OrderPool for this price level.
    uint32_t tail_order{NULL_INDEX};
};