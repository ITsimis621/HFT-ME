#pragma once
#include <cstdint>
#include <limits>

/// @brief Sentinel value representing a null or invalid index in the OrderPool.
constexpr uint32_t NULL_INDEX = std::numeric_limits<uint32_t>::max();

/// @brief Represents the market side of an order.
enum class Side : uint8_t {
    BUY  = 0, /// < Buy-side liquidity.
    SELL = 1  /// < Sell-side liquidity.
};

/// @brief Specifies the Time-In-Force (TIF) and liquidity provision behavior.
enum class OrderType : uint8_t {
    LIMIT     = 0, /// < Standard Good-Till-Canceled / Day limit order.
    IOC       = 1, /// < Immediate-Or-Cancel: fill available volume immediately, cancel remainder.
    POST_ONLY = 2, /// < Maker-Only: reject immediately if the order would take liquidity.
    FOK       = 3  /// < Fill-Or-Kill: entirely fill immediately, or cancel entirely.
};

/// @brief 32-byte cache-aligned order node designed for intrusive pools and zero-allocation processing.
/// @details Struct size and member ordering are strictly optimized to fit two instances per 64-byte cache line.
struct alignas(32) Order {
    uint64_t order_id;         /// < Unique identifier for the order (8 bytes, offset 0..7).
    uint32_t price;            /// < Limit price of the order in ticks (4 bytes, offset 8..11).
    uint32_t quantity;         /// < Remaining open quantity (4 bytes, offset 12..15).
    uint32_t prev_order;       /// < Index of the previous order in the doubly-linked price level (4 bytes, offset 16..19).
    uint32_t next_order;       /// < Index of the next order in the doubly-linked price level (4 bytes, offset 20..23).
    uint32_t participant_id;   /// < Participant identifier used for Self-Trade Prevention (STP) (4 bytes, offset 24..27).
    Side side;                 /// < Market side (1 byte, offset 28).
    OrderType type;            /// < Execution type (1 byte, offset 29).
    uint8_t padding_[2]{0, 0}; /// < Explicit padding to ensure strict 32-byte alignment (2 bytes, offset 30..31).
};

static_assert(sizeof(Order) == 32, "Order must be exactly 32 bytes (2 per cache line)");
static_assert(alignof(Order) == 32, "Order must be 32-byte aligned");