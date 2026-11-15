#pragma once
#include "Order.h"
#include "OrderPool.h"
#include "PriceLevel.h"
#include "FlatHashMap.h"
#include "SPSCQueue.h"
#include <array>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <algorithm>

/// @brief Defines the lifecycle events emitted by the matching engine.
enum class EventType : uint8_t {
    ACCEPTED = 0, /// < Validated and entered the matching engine pipeline.
    REJECTED = 1, /// < Rejected pre-trade (invalid params, duplicate ID, pool full, or POST_ONLY cross).
    EXECUTED = 2, /// < Match/Fill occurred between resting_order_id and order_id (incoming).
    CANCELED = 3, /// < Order canceled (user request, IOC/FOK unfilled remainder, or STP Cancel-Oldest).
    REDUCED  = 4, /// < Order quantity partially reduced in-place via user request.
    RESTED   = 5  /// < Remainder of a LIMIT or POST_ONLY order added to the resting book.
};

/// @brief Granular reasons for pre-trade order rejection.
enum class RejectReason : uint8_t {
    NONE               = 0,
    INVALID_PARAMETERS = 1, /// < ID was 0, price out of bounds, or quantity was 0.
    DUPLICATE_ORDER_ID = 2, /// < Order ID already exists in the flat hash map.
    CAPACITY_EXHAUSTED = 3, /// < OrderPool or FlatHashMap reached maximum capacity.
    POST_ONLY_CROSS    = 4, /// < POST_ONLY order would have crossed the spread and taken liquidity.
    BACKPRESSURE       = 5  /// < Outbound queue lacks capacity to process the order.
};

/// @brief Granular reasons for post-trade or pre-trade order cancellation.
enum class CancelReason : uint8_t {
    NONE                  = 0,
    USER_REQUESTED        = 1, /// < Explicitly canceled by the user.
    IOC_UNFILLED          = 2, /// < Remainder of an Immediate-Or-Cancel order after partial fills.
    SELF_TRADE_PREVENTION = 3, /// < Resting order canceled to prevent self-matching (Cancel-Oldest).
    FOK_UNFILLED          = 4  /// < Fill-Or-Kill order canceled because full liquidity was unavailable.
};

/// @brief Unified 32-byte outbound telemetry event.
/// @details Designed to fit exactly two events per 64-byte L1 cache line, ensuring 
/// maximum memory bandwidth utilization for the outbound SPSC queue.
struct alignas(32) OutboundEvent {
    uint64_t order_id;          /// < Primary ID: incoming order (ACCEPTED/REJECTED/EXECUTED/RESTED) or target (CANCELED/REDUCED).
    uint64_t resting_order_id;  /// < Populated on EXECUTED (maker ID) or STP CANCELED (aggressor ID). 0 otherwise.
    uint32_t price;             /// < Execution price, resting price, or canceled price.
    uint32_t quantity;          /// < Fill qty, rested qty, canceled qty, or new reduced qty.
    uint32_t participant_id;    /// < Owner of the primary order_id.
    EventType type;             /// < Discriminator for the event lifecycle.
    Side side;                  /// < Market side of the primary order_id.
    uint8_t reason;             /// < Cast from RejectReason or CancelReason.
    uint8_t padding_{0};        /// < Explicit alignment padding to strictly reach 32 bytes.
};

static_assert(sizeof(OutboundEvent) == 32, "OutboundEvent must be exactly 32 bytes");

/// @brief Standardized queue alias for outbound telemetry.
using OutboundQueue = SPSCQueue<OutboundEvent, 65536>;

constexpr uint32_t MAX_PRICE_TICKS = 200000; /// < Defines the maximum allowed price tick (e.g., $2000.00 at 1 cent ticks).
constexpr size_t MAX_ORDERS = 1000000;       /// < Maximum resting orders supported by the OrderPool.

/// @brief Core C++20 HFT Limit Order Book (LOB).
/// @details Implements a flat-memory, zero-allocation limit order book using intrusive LIFO lists, 
/// hierarchical 64-bit bitmasks for O(1) top-of-book discovery, and integrated self-trade prevention (STP).
class LimitOrderBook {
private:
    OrderPool<MAX_ORDERS> pool_;
    std::array<PriceLevel, MAX_PRICE_TICKS> bids_;
    std::array<PriceLevel, MAX_PRICE_TICKS> asks_;

    /// @brief L1 Bitmasks: 1 bit represents 1 price tick.
    std::array<uint64_t, (MAX_PRICE_TICKS / 64) + 1> bid_mask_{0};
    std::array<uint64_t, (MAX_PRICE_TICKS / 64) + 1> ask_mask_{0};

    /// @brief L2 Summary Masks: 1 bit represents 64 ticks (one full uint64_t from the L1 mask).
    /// @details Allows skipping 4096 empty ticks in a single hardware instruction during deep book sweeps.
    std::array<uint64_t, (MAX_PRICE_TICKS / 4096) + 1> bid_summary_mask_{0};
    std::array<uint64_t, (MAX_PRICE_TICKS / 4096) + 1> ask_summary_mask_{0};

    static constexpr size_t HASH_MAP_CAPACITY = 2097152;
    FlatHashMap<HASH_MAP_CAPACITY> order_map_;

    uint32_t best_bid_{0};               /// < Cached pointer to the highest bid price.
    uint32_t best_ask_{MAX_PRICE_TICKS}; /// < Cached pointer to the lowest ask price.

    OutboundQueue* outbound_queue_{nullptr};

    /// @brief Emits a telemetry event to the outbound queue via a spin-wait loop.
    inline void emit_event(const OutboundEvent& ev) noexcept {
        if (outbound_queue_) [[likely]] {
            while (!outbound_queue_->push(ev)) {} // Spin-wait on backpressure
        }
    }

public:
    /// @brief Initializes the book and pre-faults memory pages to prevent runtime OS page faults.
    explicit LimitOrderBook(OutboundQueue* outbound_queue = nullptr) 
        : outbound_queue_(outbound_queue) {
        
        // Pre-fault physical OS pages while preserving head_order = NULL_INDEX (0xFFFFFFFF)
        bids_.fill(PriceLevel{});
        asks_.fill(PriceLevel{});
    }

    /// @brief Runtime dispatch for adding an order. Identifies the side and routes to the templated engine.
    /// @param order_id Unique identifier for the incoming order.
    /// @param price Limit price of the order.
    /// @param quantity Size of the order.
    /// @param side Market side (BUY or SELL).
    /// @param type Time-In-Force or order behavior (default: LIMIT).
    /// @param participant_id Identifier for Self-Trade Prevention (default: 0).
    /// @return True if the order was accepted or successfully processed, false if rejected.
    bool add_order(uint64_t order_id, uint32_t price, uint32_t quantity, Side side,
                   OrderType type = OrderType::LIMIT, uint32_t participant_id = 0) {
        if (side == Side::BUY) {
            return add_order<Side::BUY>(order_id, price, quantity, type, participant_id);
        } else {
            return add_order<Side::SELL>(order_id, price, quantity, type, participant_id);
        }
    }

    /// @brief Compile-time templated core matching engine loop.
    /// @details Eliminates branch prediction penalties by generating independent BUY and SELL code paths.
    /// @tparam S The market side of the incoming order.
    /// @param order_id Unique identifier for the incoming order.
    /// @param price Limit price of the order.
    /// @param quantity Size of the order.
    /// @param type Time-In-Force or order behavior (default: LIMIT).
    /// @param participant_id Identifier for Self-Trade Prevention (default: 0).
    /// @return True if the order was accepted or successfully processed, false if rejected.
    template <Side S>
    bool add_order(uint64_t order_id, uint32_t price, uint32_t quantity,
                   OrderType type = OrderType::LIMIT, uint32_t participant_id = 0) {
        
        // 1. Input & Sentinel Validation
        if (order_id == 0 || price == 0 || price >= MAX_PRICE_TICKS || quantity == 0) [[unlikely]] {
            emit_event({order_id, 0, price, quantity, participant_id, EventType::REJECTED, S,
                        static_cast<uint8_t>(RejectReason::INVALID_PARAMETERS), 0});
            return false;
        }

        // 2. Duplicate ID Validation
        if (order_map_.get(order_id) != NULL_INDEX) [[unlikely]] {
            emit_event({order_id, 0, price, quantity, participant_id, EventType::REJECTED, S,
                        static_cast<uint8_t>(RejectReason::DUPLICATE_ORDER_ID), 0});
            return false;
        }

        // 3. Pre-Trade Resource Validation
        // Guarantees the Maker phase cannot fail after partial Taker executions occur
        if (pool_.is_full() || order_map_.is_full()) [[unlikely]] {
            emit_event({order_id, 0, price, quantity, participant_id, EventType::REJECTED, S,
                        static_cast<uint8_t>(RejectReason::CAPACITY_EXHAUSTED), 0});
            return false;
        }

        // 4. POST_ONLY Pre-Trade Crossing Check
        if (type == OrderType::POST_ONLY) {
            bool would_cross = false;
            if constexpr (S == Side::BUY) {
                would_cross = (price >= best_ask_);
            } else {
                would_cross = (best_bid_ != 0 && price <= best_bid_);
            }
            if (would_cross) {
                emit_event({order_id, 0, price, quantity, participant_id, EventType::REJECTED, S,
                            static_cast<uint8_t>(RejectReason::POST_ONLY_CROSS), 0});
                return false;
            }
        }

        emit_event({order_id, 0, price, quantity, participant_id, EventType::ACCEPTED, S, 0, 0});

        // 5. FOK Pre-Trade Liquidity Scan
        if (type == OrderType::FOK) {
            if (!has_sufficient_liquidity<S>(price, quantity, participant_id)) {
                emit_event({order_id, 0, price, quantity, participant_id, EventType::CANCELED, S,
                            static_cast<uint8_t>(CancelReason::FOK_UNFILLED), 0});
                return true;
            }
        }

        uint32_t remaining_qty = quantity;

        // 6. Taker Phase: Matching Logic
        auto can_match = [&]() -> bool {
            if constexpr (S == Side::BUY) {
                return remaining_qty > 0 && best_ask_ <= price;
            } else {
                return remaining_qty > 0 && best_bid_ >= price && best_bid_ != 0;
            }
        };

        while (can_match()) {
            const uint32_t match_price = (S == Side::BUY) ? best_ask_ : best_bid_;
            PriceLevel& opposite_level = (S == Side::BUY) ? asks_[match_price] : bids_[match_price];

            if (opposite_level.head_order == NULL_INDEX) [[unlikely]] break;

            uint32_t current_order_idx = opposite_level.head_order;

            while (current_order_idx != NULL_INDEX && remaining_qty > 0) {
                Order& resting_order = pool_.get(current_order_idx);
                uint32_t next_order = resting_order.next_order;

                // Self-Trade Prevention (STP - Cancel Resting / Cancel Oldest)
                if (participant_id != 0 && resting_order.participant_id == participant_id) [[unlikely]] {
                    uint64_t canceled_id = resting_order.order_id;
                    uint32_t canceled_qty = resting_order.quantity;
                    Side resting_side = resting_order.side;

                    remove_order_by_index(current_order_idx);

                    emit_event({canceled_id, order_id, match_price, canceled_qty, participant_id,
                                EventType::CANCELED, resting_side,
                                static_cast<uint8_t>(CancelReason::SELF_TRADE_PREVENTION), 0});

                    current_order_idx = next_order;
                    continue;
                }

                uint32_t fill_qty = std::min(remaining_qty, resting_order.quantity);

                emit_event({order_id, resting_order.order_id, match_price, fill_qty, participant_id,
                            EventType::EXECUTED, S, 0, 0});

                remaining_qty -= fill_qty;
                resting_order.quantity -= fill_qty;
                opposite_level.total_volume -= fill_qty;

                if (resting_order.quantity == 0) {
                    remove_order_by_index(current_order_idx);
                }

                current_order_idx = next_order;
            }
        }

        // 7. Maker Phase: Post-Match & Adding Logic
        if (remaining_qty == 0) return true;

        if (type == OrderType::IOC || type == OrderType::FOK) {
            emit_event({order_id, 0, price, remaining_qty, participant_id, EventType::CANCELED, S,
                        static_cast<uint8_t>(type == OrderType::IOC ? CancelReason::IOC_UNFILLED : CancelReason::FOK_UNFILLED), 0});
            return true;
        }

        uint32_t pool_index = pool_.allocate();
        order_map_.insert(order_id, pool_index);

        Order& order = pool_.get(pool_index);
        order.order_id = order_id;
        order.price = price;
        order.quantity = remaining_qty;
        order.prev_order = NULL_INDEX;
        order.next_order = NULL_INDEX;
        order.participant_id = participant_id;
        order.side = S;
        order.type = type;

        PriceLevel& level = (S == Side::BUY) ? bids_[price] : asks_[price];

        order.prev_order = level.tail_order;

        if (level.head_order == NULL_INDEX) {
            level.head_order = pool_index;
        } else {
            pool_.get(level.tail_order).next_order = pool_index;
        }

        level.tail_order = pool_index;
        level.total_volume += remaining_qty;

        size_t chunk = price >> 6;
        size_t summary_chunk = chunk >> 6;

        if constexpr (S == Side::BUY) {
            bid_mask_[chunk] |= (1ULL << (price & 63));
            bid_summary_mask_[summary_chunk] |= (1ULL << (chunk & 63));
            if (price > best_bid_) best_bid_ = price;
        } else {
            ask_mask_[chunk] |= (1ULL << (price & 63));
            ask_summary_mask_[summary_chunk] |= (1ULL << (chunk & 63));
            if (price < best_ask_) best_ask_ = price;
        }

        emit_event({order_id, 0, price, remaining_qty, participant_id, EventType::RESTED, S, 0, 0});

        return true;
    }

    /// @brief Cancels an active order completely based on its ID.
    /// @param order_id The ID of the order to cancel.
    /// @return True if canceled, false if the order ID was not found.
    bool cancel_order(uint64_t order_id) {
        uint32_t pool_index = order_map_.get(order_id);
        if (pool_index == NULL_INDEX) [[unlikely]] {
            return false;
        }

        const Order& order = pool_.get(pool_index);
        uint32_t price = order.price;
        uint32_t qty = order.quantity;
        uint32_t pid = order.participant_id;
        Side side = order.side;

        remove_order_by_index(pool_index);

        emit_event({order_id, 0, price, qty, pid, EventType::CANCELED, side,
                    static_cast<uint8_t>(CancelReason::USER_REQUESTED), 0});
        return true;
    }

    /// @brief Modifies the volume of an existing order down to a lower quantity in-place.
    /// @details Increasing volume is unsupported to prevent queue jumping (requires cancel + replace).
    /// @param order_id The ID of the order to reduce.
    /// @param new_quantity The targeted new total quantity (must be < current quantity).
    /// @return True on success, false if ID not found, or quantity is >= current.
    bool reduce_order(uint64_t order_id, uint32_t new_quantity) {
        uint32_t pool_index = order_map_.get(order_id);
        if (pool_index == NULL_INDEX) [[unlikely]] return false;

        Order& order = pool_.get(pool_index);
        if (new_quantity >= order.quantity) [[unlikely]] return false;

        uint32_t price = order.price;
        uint32_t pid = order.participant_id;
        Side side = order.side;

        if (new_quantity == 0) [[unlikely]] {
            uint32_t canceled_qty = order.quantity;
            remove_order_by_index(pool_index);
            emit_event({order_id, 0, price, canceled_qty, pid, EventType::CANCELED, side,
                        static_cast<uint8_t>(CancelReason::USER_REQUESTED), 0});
            return true;
        }

        uint32_t diff = order.quantity - new_quantity;
        order.quantity = new_quantity;

        PriceLevel& level = (side == Side::BUY) ? bids_[price] : asks_[price];
        level.total_volume -= diff;

        emit_event({order_id, 0, price, new_quantity, pid, EventType::REDUCED, side, 0, 0});
        return true;
    }

    // ------------------------------------------------------------------------
    // Public Observers & Invariant Checker
    // ------------------------------------------------------------------------
    
    /// @brief Retrieves the current highest bid price.
    uint32_t best_bid() const noexcept { return best_bid_; }
    
    /// @brief Retrieves the current lowest ask price.
    uint32_t best_ask() const noexcept { return best_ask_; }

    /// @brief Fetches the total resting volume at a specific price tick.
    uint64_t get_volume_at(Side side, uint32_t price) const noexcept {
        if (price == 0 || price >= MAX_PRICE_TICKS) return 0;
        return (side == Side::BUY) ? bids_[price].total_volume : asks_[price].total_volume;
    }

    /// @brief Deep diagnostic routine to verify memory and logic invariants.
    /// @details Validates doubly-linked lists, aggregate volume, bitmask integrity, and flat map mappings.
    /// Use exclusively in unit testing.
    /// @return True if the internal structures are completely healthy.
    bool verify_invariants() const noexcept {
        uint32_t expected_best_bid = 0;
        uint32_t expected_best_ask = MAX_PRICE_TICKS;

        for (uint32_t p = 1; p < MAX_PRICE_TICKS; ++p) {
            size_t chunk = p >> 6;
            uint64_t bit = 1ULL << (p & 63);

            // Verify BID level p
            const PriceLevel& bid_lvl = bids_[p];
            bool bid_bit_set = (bid_mask_[chunk] & bit) != 0;
            bool bid_has_orders = (bid_lvl.head_order != NULL_INDEX);

            if (bid_bit_set != bid_has_orders) return false;

            if (!bid_has_orders) {
                if (bid_lvl.tail_order != NULL_INDEX || bid_lvl.total_volume != 0) return false;
            } else {
                expected_best_bid = p;
                uint64_t sum_vol = 0;
                uint32_t curr = bid_lvl.head_order;
                uint32_t prev = NULL_INDEX;
                while (curr != NULL_INDEX) {
                    const Order& o = pool_.get(curr);
                    if (o.price != p || o.side != Side::BUY) return false;
                    if (o.prev_order != prev) return false;
                    if (order_map_.get(o.order_id) != curr) return false;
                    sum_vol += o.quantity;
                    prev = curr;
                    curr = o.next_order;
                }
                if (prev != bid_lvl.tail_order) return false;
                if (sum_vol != bid_lvl.total_volume) return false;
            }

            // Verify ASK level p
            const PriceLevel& ask_lvl = asks_[p];
            bool ask_bit_set = (ask_mask_[chunk] & bit) != 0;
            bool ask_has_orders = (ask_lvl.head_order != NULL_INDEX);

            if (ask_bit_set != ask_has_orders) return false;

            if (!ask_has_orders) {
                if (ask_lvl.tail_order != NULL_INDEX || ask_lvl.total_volume != 0) return false;
            } else {
                if (p < expected_best_ask) expected_best_ask = p;
                uint64_t sum_vol = 0;
                uint32_t curr = ask_lvl.head_order;
                uint32_t prev = NULL_INDEX;
                while (curr != NULL_INDEX) {
                    const Order& o = pool_.get(curr);
                    if (o.price != p || o.side != Side::SELL) return false;
                    if (o.prev_order != prev) return false;
                    if (order_map_.get(o.order_id) != curr) return false;
                    sum_vol += o.quantity;
                    prev = curr;
                    curr = o.next_order;
                }
                if (prev != ask_lvl.tail_order) return false;
                if (sum_vol != ask_lvl.total_volume) return false;
            }
        }

        // Verify L2 summary masks match L1 base masks
        for (size_t c = 0; c < bid_mask_.size(); ++c) {
            size_t s = c >> 6;
            uint64_t s_bit = 1ULL << (c & 63);
            if (((bid_summary_mask_[s] & s_bit) != 0) != (bid_mask_[c] != 0)) return false;
            if (((ask_summary_mask_[s] & s_bit) != 0) != (ask_mask_[c] != 0)) return false;
        }

        return (best_bid_ == expected_best_bid) && (best_ask_ == expected_best_ask);
    }

private:
    /// @brief Stage 1 FOK Validation: Hardware-accelerated aggregate volume check.
    /// @details Uses SIMD `countl_zero`/`countr_zero` to jump directly across populated price levels 
    /// without iterating over empty ticks or fetching from the OrderPool.
    /// @tparam S The market side of the incoming FOK order (Taker side).
    /// @param limit_price The maximum (BUY) or minimum (SELL) price the Taker is willing to accept.
    /// @param quantity The total quantity required to fill the FOK order.
    /// @return True if sufficient aggregate volume exists at or better than the limit price.
    template <Side S>
    bool has_raw_liquidity(uint32_t limit_price, uint32_t quantity) const noexcept {
        uint32_t needed = quantity;

        if constexpr (S == Side::BUY) {
            if (best_ask_ > limit_price || best_ask_ == MAX_PRICE_TICKS) return false;

            // Fast Path: Top of book alone has enough raw volume
            if (asks_[best_ask_].total_volume >= needed) return true;
            if (best_ask_ == limit_price) return false;

            size_t start_chunk = best_ask_ >> 6;
            size_t end_chunk = limit_price >> 6;

            for (size_t c = start_chunk; c <= end_chunk; ++c) {
                uint64_t mask = ask_mask_[c];

                if (c == start_chunk) mask &= ~((1ULL << (best_ask_ & 63)) - 1);
                if (c == end_chunk && (limit_price & 63) != 63) {
                    mask &= ((1ULL << ((limit_price & 63) + 1)) - 1);
                }

                while (mask != 0) {
                    int bit = std::countr_zero(mask);
                    uint32_t p = (static_cast<uint32_t>(c) * 64) + bit;
                    if (asks_[p].total_volume >= needed) return true;
                    needed -= static_cast<uint32_t>(asks_[p].total_volume);
                    mask &= mask - 1;
                }
            }
            return false;
        } else {
            if (best_bid_ < limit_price || best_bid_ == 0) return false;

            // Fast Path: Top of book alone has enough raw volume
            if (bids_[best_bid_].total_volume >= needed) return true;
            if (best_bid_ == limit_price) return false;

            size_t start_chunk = best_bid_ >> 6;
            size_t end_chunk = limit_price >> 6;

            for (int c = static_cast<int>(start_chunk); c >= static_cast<int>(end_chunk); --c) {
                uint64_t mask = bid_mask_[c];

                if (c == static_cast<int>(start_chunk) && (best_bid_ & 63) != 63) {
                    mask &= ((1ULL << ((best_bid_ & 63) + 1)) - 1);
                }
                if (c == static_cast<int>(end_chunk)) mask &= ~((1ULL << (limit_price & 63)) - 1);

                while (mask != 0) {
                    int bit = 63 - std::countl_zero(mask);
                    uint32_t p = (static_cast<uint32_t>(c) * 64) + bit;
                    if (bids_[p].total_volume >= needed) return true;
                    needed -= static_cast<uint32_t>(bids_[p].total_volume);
                    mask &= ~(1ULL << bit);
                }
            }
            return false;
        }
    }

    /// @brief Stage 2 FOK Validation: STP-aware granular liquidity verification.
    /// @details Only executes if raw volume passes. Iterates through the intrusive linked lists 
    /// to exclude volume tied to the caller's own participant_id. Warms L1 cache for the Taker loop on success.
    /// @tparam S The market side of the incoming FOK order (Taker side).
    /// @param limit_price The maximum (BUY) or minimum (SELL) price the Taker is willing to accept.
    /// @param quantity The total quantity required to fill the FOK order.
    /// @param participant_id The ID of the Taker, used to subtract self-trade volume from available liquidity.
    /// @return True if sufficient non-STP liquidity exists at or better than the limit price.
    template <Side S>
    bool has_sufficient_liquidity(uint32_t limit_price, uint32_t quantity, uint32_t participant_id) const noexcept {
        if (!has_raw_liquidity<S>(limit_price, quantity)) {
            return false;
        }

        if (participant_id == 0) {
            return true;
        }

        uint32_t needed = quantity;

        if constexpr (S == Side::BUY) {
            size_t start_chunk = best_ask_ >> 6;
            size_t end_chunk = limit_price >> 6;

            for (size_t c = start_chunk; c <= end_chunk; ++c) {
                uint64_t mask = ask_mask_[c];

                if (c == start_chunk) mask &= ~((1ULL << (best_ask_ & 63)) - 1);
                if (c == end_chunk && (limit_price & 63) != 63) {
                    mask &= ((1ULL << ((limit_price & 63) + 1)) - 1);
                }

                while (mask != 0) {
                    int bit = std::countr_zero(mask);
                    uint32_t p = (static_cast<uint32_t>(c) * 64) + bit;
                    uint64_t next_mask = mask & ~(1ULL << bit);
                    bool is_last_possible_level = (c == end_chunk) && (next_mask == 0);

                    uint64_t level_avail = asks_[p].total_volume;
                    if (is_last_possible_level && level_avail < needed) return false;

                    uint32_t curr = asks_[p].head_order;
                    while (curr != NULL_INDEX) {
                        const Order& o = pool_.get(curr);
                        if (o.participant_id != participant_id) [[likely]] {
                            if (o.quantity >= needed) return true;
                            needed -= o.quantity;
                        } else {
                            level_avail -= o.quantity;
                            if (is_last_possible_level && level_avail < needed) return false;
                        }
                        curr = o.next_order;
                    }

                    mask = next_mask;
                }
            }
            return false;
        } else {
            size_t start_chunk = best_bid_ >> 6;
            size_t end_chunk = limit_price >> 6;

            for (int c = static_cast<int>(start_chunk); c >= static_cast<int>(end_chunk); --c) {
                uint64_t mask = bid_mask_[c];

                if (c == static_cast<int>(start_chunk) && (best_bid_ & 63) != 63) {
                    mask &= ((1ULL << ((best_bid_ & 63) + 1)) - 1);
                }
                if (c == static_cast<int>(end_chunk)) mask &= ~((1ULL << (limit_price & 63)) - 1);

                while (mask != 0) {
                    int bit = 63 - std::countl_zero(mask);
                    uint32_t p = (static_cast<uint32_t>(c) * 64) + bit;
                    uint64_t next_mask = mask & ~(1ULL << bit);
                    bool is_last_possible_level = (c == static_cast<int>(end_chunk)) && (next_mask == 0);

                    uint64_t level_avail = bids_[p].total_volume;
                    if (is_last_possible_level && level_avail < needed) return false;

                    uint32_t curr = bids_[p].head_order;
                    while (curr != NULL_INDEX) {
                        const Order& o = pool_.get(curr);
                        if (o.participant_id != participant_id) [[likely]] {
                            if (o.quantity >= needed) return true;
                            needed -= o.quantity;
                        } else {
                            level_avail -= o.quantity;
                            if (is_last_possible_level && level_avail < needed) return false;
                        }
                        curr = o.next_order;
                    }

                    mask = next_mask;
                }
            }
            return false;
        }
    }

    /// @brief Detaches an order from the active doubly-linked list and returns it to the free pool.
    /// @details Updates the top-of-book pointers and bitmasks if the level becomes completely exhausted.
    inline void remove_order_by_index(uint32_t pool_index) noexcept {
        Order& order = pool_.get(pool_index);
        PriceLevel& level = (order.side == Side::BUY) ? bids_[order.price] : asks_[order.price];

        if (order.prev_order != NULL_INDEX) {
            pool_.get(order.prev_order).next_order = order.next_order;
        } else {
            level.head_order = order.next_order;
        }

        if (order.next_order != NULL_INDEX) {
            pool_.get(order.next_order).prev_order = order.prev_order;
        } else {
            level.tail_order = order.prev_order;
        }

        level.total_volume -= order.quantity;

        Side side = order.side;
        uint32_t price = order.price;

        order_map_.remove(order.order_id);
        pool_.deallocate(pool_index);

        if (side == Side::BUY) {
            update_top_of_book<Side::BUY>(price, level);
        } else {
            update_top_of_book<Side::SELL>(price, level);
        }
    }

    /// @brief Re-evaluates top-of-book pointers when an entire price level is deleted.
    /// @details Uses hierarchical L2 summary masks for O(1) jump-scans across empty price bands.
    template <Side S>
    void update_top_of_book(uint32_t price, const PriceLevel& level) noexcept {
        if (level.head_order != NULL_INDEX) return;

        size_t chunk = price >> 6;
        size_t summary_chunk = chunk >> 6;
        uint64_t bit = 1ULL << (price & 63);

        if constexpr (S == Side::BUY) {
            bid_mask_[chunk] &= ~bit;
            if (bid_mask_[chunk] == 0) {
                bid_summary_mask_[summary_chunk] &= ~(1ULL << (chunk & 63));
            }

            if (price != best_bid_) return;

            for (int s = static_cast<int>(summary_chunk); s >= 0; --s) {
                if (bid_summary_mask_[s] != 0) {
                    size_t highest_chunk = (s * 64) + (63 - std::countl_zero(bid_summary_mask_[s]));
                    best_bid_ = static_cast<uint32_t>(
                        (highest_chunk * 64) + (63 - std::countl_zero(bid_mask_[highest_chunk]))
                    );
                    return;
                }
            }
            best_bid_ = 0;
        } else {
            ask_mask_[chunk] &= ~bit;
            if (ask_mask_[chunk] == 0) {
                ask_summary_mask_[summary_chunk] &= ~(1ULL << (chunk & 63));
            }

            if (price != best_ask_) return;

            for (size_t s = summary_chunk; s < ask_summary_mask_.size(); ++s) {
                if (ask_summary_mask_[s] != 0) {
                    size_t lowest_chunk = (s * 64) + std::countr_zero(ask_summary_mask_[s]);
                    best_ask_ = static_cast<uint32_t>(
                        (lowest_chunk * 64) + std::countr_zero(ask_mask_[lowest_chunk])
                    );
                    return;
                }
            }
            best_ask_ = MAX_PRICE_TICKS;
        }
    }
};