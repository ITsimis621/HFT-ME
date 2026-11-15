#include "engine/LimitOrderBook.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

/// @brief Custom assertion macro for the verification suite.
/// @details Designed to work cleanly under `-fno-exceptions` and `-Werror` environments 
/// where standard `<cassert>` may be stripped out by compiler flags like `-DNDEBUG`.
#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) [[unlikely]] { \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " (" << __func__ << "): " << (msg) << "\n"; \
            std::abort(); \
        } \
    } while (0)

/// @brief Scoped context for managing the lifecycle of the engine during isolated test cases.
struct TestContext {
    std::unique_ptr<OutboundQueue> queue;
    std::unique_ptr<LimitOrderBook> lob;

    TestContext()
        : queue(std::make_unique<OutboundQueue>()),
          lob(std::make_unique<LimitOrderBook>(queue.get())) {}

    /// @brief Drains the outbound queue and filters exclusively for EXECUTED events.
    /// @return A vector of execution telemetry records.
    std::vector<OutboundEvent> drain_executions() {
        std::vector<OutboundEvent> records;
        OutboundEvent rec{};
        while (queue->pop(rec)) {
            if (rec.type == EventType::EXECUTED) {
                records.push_back(rec);
            }
        }
        return records;
    }

    /// @brief Silently drops all pending telemetry events from the outbound queue.
    void clear_events() {
        OutboundEvent rec{};
        while (queue->pop(rec)) {}
    }
};

// ==========================================
// FOUNDATIONAL MECHANICAL TESTS
// ==========================================

void test_startup_and_bounds() {
    TestContext ctx;
    auto& lob = *ctx.lob;

    TEST_ASSERT(lob.best_bid() == 0, "Initial best bid");
    TEST_ASSERT(lob.best_ask() == MAX_PRICE_TICKS, "Initial best ask");
    TEST_ASSERT(lob.verify_invariants(), "Initial invariants");

    // Reject invalid inputs
    TEST_ASSERT(!lob.add_order(0, 100, 10, Side::BUY), "Reject order_id 0");
    TEST_ASSERT(!lob.add_order(1, 0, 10, Side::BUY), "Reject price 0");
    TEST_ASSERT(!lob.add_order(1, MAX_PRICE_TICKS, 10, Side::BUY), "Reject price >= MAX");
    TEST_ASSERT(!lob.add_order(1, MAX_PRICE_TICKS + 500, 10, Side::SELL), "Reject price out of bounds");
    TEST_ASSERT(!lob.add_order(1, 100, 0, Side::BUY), "Reject qty 0");

    // Accept valid bounds
    TEST_ASSERT(lob.add_order(1, 1, 25, Side::BUY), "Accept lowest valid tick");
    TEST_ASSERT(lob.add_order(2, MAX_PRICE_TICKS - 1, 40, Side::SELL), "Accept highest valid tick");
    TEST_ASSERT(lob.best_bid() == 1, "Best bid is 1");
    TEST_ASSERT(lob.best_ask() == MAX_PRICE_TICKS - 1, "Best ask is MAX-1");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after bounds test");

    std::cout << "[PASS] test_startup_and_bounds\n";
}

void test_duplicate_order_id_rejection() {
    TestContext ctx;
    auto& lob = *ctx.lob;

    TEST_ASSERT(lob.add_order(100, 500, 50, Side::BUY), "Add initial order");
    TEST_ASSERT(!lob.add_order(100, 505, 20, Side::BUY), "Reject duplicate ID same side");
    TEST_ASSERT(!lob.add_order(100, 490, 20, Side::SELL), "Reject duplicate ID cross side BEFORE Taker phase");

    TEST_ASSERT(lob.best_bid() == 500, "Best bid unaffected");
    TEST_ASSERT(lob.get_volume_at(Side::BUY, 500) == 50, "Volume unaffected");
    TEST_ASSERT(ctx.drain_executions().empty(), "No executions occurred");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after duplicate rejection");

    std::cout << "[PASS] test_duplicate_order_id_rejection\n";
}

void test_bitmask_hierarchy_boundaries() {
    TestContext ctx;
    auto& lob = *ctx.lob;

    // Test cross-chunk bitmask logic (tick 63 to 64 crosses an L1 chunk boundary)
    const std::vector<uint32_t> prices = {1, 63, 64, 65, 4095, 4096, 4097, 8192, 199999};
    uint64_t id = 1;
    for (uint32_t p : prices) {
        TEST_ASSERT(lob.add_order(id++, p, 10, Side::BUY), "Add bid boundary");
    }
    TEST_ASSERT(lob.best_bid() == 199999, "Highest bid verified");
    TEST_ASSERT(lob.verify_invariants(), "Invariants with bid boundaries");

    // Test cascading deletions
    for (size_t i = prices.size(); i-- > 0;) {
        TEST_ASSERT(lob.best_bid() == prices[i], "Best bid matches boundary during cancel");
        lob.cancel_order(static_cast<uint64_t>(i + 1));
        TEST_ASSERT(lob.verify_invariants(), "Invariants during bid cancel");
    }
    TEST_ASSERT(lob.best_bid() == 0, "Book empty after bid cancels");

    id = 100;
    for (uint32_t p : prices) {
        TEST_ASSERT(lob.add_order(id++, p, 10, Side::SELL), "Add ask boundary");
    }
    TEST_ASSERT(lob.best_ask() == 1, "Lowest ask verified");
    TEST_ASSERT(lob.verify_invariants(), "Invariants with ask boundaries");

    for (size_t i = 0; i < prices.size(); ++i) {
        TEST_ASSERT(lob.best_ask() == prices[i], "Best ask matches boundary during cancel");
        lob.cancel_order(100 + i);
        TEST_ASSERT(lob.verify_invariants(), "Invariants during ask cancel");
    }
    TEST_ASSERT(lob.best_ask() == MAX_PRICE_TICKS, "Book empty after ask cancels");

    std::cout << "[PASS] test_bitmask_hierarchy_boundaries\n";
}

void test_linked_list_unlinking() {
    TestContext ctx;
    auto& lob = *ctx.lob;

    TEST_ASSERT(lob.add_order(1, 1000, 10, Side::BUY), "Order 1");
    TEST_ASSERT(lob.add_order(2, 1000, 20, Side::BUY), "Order 2");
    TEST_ASSERT(lob.add_order(3, 1000, 30, Side::BUY), "Order 3");
    TEST_ASSERT(lob.add_order(4, 1000, 40, Side::BUY), "Order 4");
    TEST_ASSERT(lob.get_volume_at(Side::BUY, 1000) == 100, "Volume 100");

    // Verify intrusive doubly-linked list edge cases
    lob.cancel_order(2); // Middle
    TEST_ASSERT(lob.get_volume_at(Side::BUY, 1000) == 80, "Volume 80 after mid cancel");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after mid cancel");

    lob.cancel_order(1); // Head
    TEST_ASSERT(lob.get_volume_at(Side::BUY, 1000) == 70, "Volume 70 after head cancel");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after head cancel");

    lob.cancel_order(4); // Tail
    TEST_ASSERT(lob.get_volume_at(Side::BUY, 1000) == 30, "Volume 30 after tail cancel");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after tail cancel");

    lob.cancel_order(3); // Last remaining node
    TEST_ASSERT(lob.get_volume_at(Side::BUY, 1000) == 0, "Volume 0");
    TEST_ASSERT(lob.best_bid() == 0, "Bid book empty");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after full cancel");

    std::cout << "[PASS] test_linked_list_unlinking\n";
}

void test_reduce_order_and_fifo_priority() {
    TestContext ctx;
    auto& lob = *ctx.lob;

    TEST_ASSERT(lob.add_order(1, 500, 50, Side::SELL), "Sell 1");
    TEST_ASSERT(lob.add_order(2, 500, 50, Side::SELL), "Sell 2");

    TEST_ASSERT(!lob.reduce_order(1, 50), "Reject reduce to same qty");
    TEST_ASSERT(!lob.reduce_order(1, 60), "Reject reduce to higher qty");
    TEST_ASSERT(!lob.reduce_order(999, 10), "Reject reduce unknown ID");

    TEST_ASSERT(lob.reduce_order(1, 15), "Reduce order 1 to 15");
    TEST_ASSERT(lob.get_volume_at(Side::SELL, 500) == 65, "Volume 65");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after reduce");

    ctx.clear_events();

    TEST_ASSERT(lob.add_order(3, 500, 15, Side::BUY), "Buy sweeps remaining priority");
    auto execs = ctx.drain_executions();
    TEST_ASSERT(execs.size() == 1, "Exactly one execution");
    TEST_ASSERT(execs[0].resting_order_id == 1, "Order 1 retains FIFO priority");
    TEST_ASSERT(execs[0].order_id == 3, "Incoming is 3");
    
    TEST_ASSERT(execs[0].price == 500, "Price 500");
    TEST_ASSERT(execs[0].quantity == 15, "Fill qty 15");

    TEST_ASSERT(lob.reduce_order(2, 0), "Reduce to 0 cancels order");
    TEST_ASSERT(lob.best_ask() == MAX_PRICE_TICKS, "Ask book empty");
    TEST_ASSERT(lob.verify_invariants(), "Final invariants");

    std::cout << "[PASS] test_reduce_order_and_fifo_priority\n";
}

void test_multi_level_sweep_and_rest() {
    TestContext ctx;
    auto& lob = *ctx.lob;

    TEST_ASSERT(lob.add_order(1, 100, 10, Side::SELL), "Ask 1");
    TEST_ASSERT(lob.add_order(2, 100, 20, Side::SELL), "Ask 2");
    TEST_ASSERT(lob.add_order(3, 102, 25, Side::SELL), "Ask 3");
    TEST_ASSERT(lob.add_order(4, 105, 50, Side::SELL), "Ask 4");
    TEST_ASSERT(lob.best_ask() == 100, "Best ask 100");

    ctx.clear_events();
    TEST_ASSERT(lob.add_order(10, 103, 70, Side::BUY), "Aggressive sweep");

    auto execs = ctx.drain_executions();
    TEST_ASSERT(execs.size() == 3, "3 partial fills");
    TEST_ASSERT(execs[0].resting_order_id == 1 && execs[0].price == 100 && execs[0].quantity == 10, "Fill 1");
    TEST_ASSERT(execs[1].resting_order_id == 2 && execs[1].price == 100 && execs[1].quantity == 20, "Fill 2");
    TEST_ASSERT(execs[2].resting_order_id == 3 && execs[2].price == 102 && execs[2].quantity == 25, "Fill 3");

    TEST_ASSERT(lob.best_bid() == 103, "Sweep rests at 103");
    TEST_ASSERT(lob.get_volume_at(Side::BUY, 103) == 15, "Remaining 15 rests");
    TEST_ASSERT(lob.best_ask() == 105, "New best ask 105");
    TEST_ASSERT(lob.get_volume_at(Side::SELL, 105) == 50, "Ask volume 50");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after sweep");

    std::cout << "[PASS] test_multi_level_sweep_and_rest\n";
}

void test_pool_and_hashmap_churn() {
    TestContext ctx;
    auto& lob = *ctx.lob;

    // Fill pool heavily
    for (uint64_t id = 1; id <= 5000; ++id) {
        uint32_t price = static_cast<uint32_t>(1000 + (id % 50));
        TEST_ASSERT(lob.add_order(id, price, 10, Side::BUY), "Add churn 1");
    }
    TEST_ASSERT(lob.verify_invariants(), "Invariants after 5000 adds");

    // Interleave deletions to fragment the intrusive stack
    for (uint64_t id = 1; id <= 5000; id += 2) {
        lob.cancel_order(id);
    }
    TEST_ASSERT(lob.verify_invariants(), "Invariants after 2500 interleaving cancels");

    // Refill to ensure allocations map cleanly over fragmented slots
    for (uint64_t id = 5001; id <= 7500; ++id) {
        uint32_t price = static_cast<uint32_t>(1000 + (id % 50));
        TEST_ASSERT(lob.add_order(id, price, 10, Side::BUY), "Add churn 2");
    }
    TEST_ASSERT(lob.verify_invariants(), "Invariants after pool reuse");

    std::cout << "[PASS] test_pool_and_hashmap_churn\n";
}

// ==========================================
// EXCHANGE DOMAIN FEATURE TESTS
// ==========================================

void test_post_only_orders() {
    TestContext ctx;
    auto& lob = *ctx.lob;
    auto& queue = *ctx.queue;

    TEST_ASSERT(lob.add_order(1, 1000, 50, Side::SELL, OrderType::POST_ONLY), "POST_ONLY sell rests");
    TEST_ASSERT(lob.best_ask() == 1000, "Best ask is 1000");

    ctx.clear_events();

    TEST_ASSERT(!lob.add_order(2, 1000, 25, Side::BUY, OrderType::POST_ONLY), "POST_ONLY crossing buy rejected");
    TEST_ASSERT(lob.get_volume_at(Side::SELL, 1000) == 50, "Resting ask volume untouched");
    TEST_ASSERT(lob.best_bid() == 0, "Best bid remains 0");

    OutboundEvent ev{};
    TEST_ASSERT(queue.pop(ev), "Should emit reject event");
    TEST_ASSERT(ev.type == EventType::REJECTED, "Event is REJECTED");
    TEST_ASSERT(ev.reason == static_cast<uint8_t>(RejectReason::POST_ONLY_CROSS), "Reason is POST_ONLY_CROSS");

    TEST_ASSERT(lob.add_order(3, 999, 25, Side::BUY, OrderType::POST_ONLY), "POST_ONLY non-crossing buy rests");
    TEST_ASSERT(lob.best_bid() == 999, "Best bid is 999");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after POST_ONLY test");

    std::cout << "[PASS] test_post_only_orders\n";
}

void test_ioc_orders() {
    TestContext ctx;
    auto& lob = *ctx.lob;
    auto& queue = *ctx.queue;

    TEST_ASSERT(lob.add_order(1, 500, 40, Side::SELL, OrderType::LIMIT), "Rest 40 at 500");
    ctx.clear_events();

    TEST_ASSERT(lob.add_order(2, 500, 100, Side::BUY, OrderType::IOC), "IOC buy returns true");
    TEST_ASSERT(lob.best_ask() == MAX_PRICE_TICKS, "Ask book now empty");
    TEST_ASSERT(lob.best_bid() == 0, "IOC remainder does NOT rest on bid book");

    OutboundEvent ev{};
    TEST_ASSERT(queue.pop(ev) && ev.type == EventType::ACCEPTED && ev.order_id == 2, "IOC Accepted");
    TEST_ASSERT(queue.pop(ev) && ev.type == EventType::EXECUTED && ev.quantity == 40, "IOC Partial Fill 40");
    TEST_ASSERT(queue.pop(ev) && ev.type == EventType::CANCELED && ev.order_id == 2 &&
                ev.quantity == 60 && ev.reason == static_cast<uint8_t>(CancelReason::IOC_UNFILLED),
                "IOC Unfilled Remainder Canceled");
    TEST_ASSERT(queue.empty(), "Queue empty after IOC sequence");
    TEST_ASSERT(lob.verify_invariants(), "Invariants after IOC test");

    std::cout << "[PASS] test_ioc_orders\n";
}

void test_fok_with_stp() {
    auto queue = std::make_unique<OutboundQueue>();
    auto lob = std::make_unique<LimitOrderBook>(queue.get());
    OutboundEvent ev{}; 

    // Setup: Price 100 has 50 shares from Participant 42 (self) and 60 shares from Participant 99
    // Total raw volume at 100 = 110, but fillable volume for Participant 42 = 60.
    TEST_ASSERT(lob->add_order(1, 100, 50, Side::SELL, OrderType::LIMIT, 42), "Setup order 1");
    TEST_ASSERT(lob->add_order(2, 100, 60, Side::SELL, OrderType::LIMIT, 99), "Setup order 2");
    while (queue->pop(ev)) {} 

    // Case 1: Participant 42 sends FOK BUY for 100 @ 100.
    // Raw volume (110) >= 100, but non-self volume (60) < 100.
    // Must be killed pre-trade WITHOUT canceling resting Order 1 or filling Order 2.
    TEST_ASSERT(lob->add_order(3, 100, 100, Side::BUY, OrderType::FOK, 42), "FOK Buy 1");
    TEST_ASSERT(lob->get_volume_at(Side::SELL, 100) == 110, "Sell volume unchanged");

    TEST_ASSERT(queue->pop(ev) && ev.type == EventType::ACCEPTED && ev.order_id == 3, "FOK 1 ACCEPTED");
    TEST_ASSERT(queue->pop(ev) && ev.type == EventType::CANCELED &&
                ev.order_id == 3 && ev.reason == static_cast<uint8_t>(CancelReason::FOK_UNFILLED), "FOK 1 CANCELED");
    TEST_ASSERT(!queue->pop(ev), "Queue empty");
    TEST_ASSERT(lob->verify_invariants(), "Invariants 1");

    // Case 2: Add 40 shares from Participant 88 at Price 101.
    // Non-self volume for Participant 42 across [100, 101] is now 60 + 40 = 100.
    TEST_ASSERT(lob->add_order(4, 101, 40, Side::SELL, OrderType::LIMIT, 88), "Setup order 4");
    while (queue->pop(ev)) {}

    // Participant 42 sends FOK BUY for 100 @ 101.
    // Must succeed: STP cancels resting Order 1 (50 @ 100), executes Order 2 (60 @ 100) and Order 4 (40 @ 101).
    TEST_ASSERT(lob->add_order(5, 101, 100, Side::BUY, OrderType::FOK, 42), "FOK Buy 2");
    TEST_ASSERT(lob->get_volume_at(Side::SELL, 100) == 0, "Level 1 cleared");
    TEST_ASSERT(lob->get_volume_at(Side::SELL, 101) == 0, "Level 2 cleared");
    TEST_ASSERT(lob->best_ask() == MAX_PRICE_TICKS, "Book empty");

    TEST_ASSERT(queue->pop(ev) && ev.type == EventType::ACCEPTED && ev.order_id == 5, "FOK 2 ACCEPTED");
    TEST_ASSERT(queue->pop(ev) && ev.type == EventType::CANCELED &&
                ev.order_id == 1 && ev.resting_order_id == 5 &&
                ev.reason == static_cast<uint8_t>(CancelReason::SELF_TRADE_PREVENTION), "STP CANCELED");
    TEST_ASSERT(queue->pop(ev) && ev.type == EventType::EXECUTED &&
                ev.order_id == 5 && ev.resting_order_id == 2 && ev.quantity == 60, "Fill 1");
    TEST_ASSERT(queue->pop(ev) && ev.type == EventType::EXECUTED &&
                ev.order_id == 5 && ev.resting_order_id == 4 && ev.quantity == 40, "Fill 2");

    TEST_ASSERT(!queue->pop(ev), "Queue empty 2");
    TEST_ASSERT(lob->verify_invariants(), "Invariants 2");
    
    std::cout << "[PASS] test_fok_with_stp\n";
}

void test_self_trade_prevention() {
    TestContext ctx;
    auto& lob = *ctx.lob;
    auto& queue = *ctx.queue;

    TEST_ASSERT(lob.add_order(1, 1000, 30, Side::SELL, OrderType::LIMIT, 42), "P42 sell rests");
    TEST_ASSERT(lob.add_order(2, 1000, 50, Side::SELL, OrderType::LIMIT, 99), "P99 sell rests");
    ctx.clear_events();

    TEST_ASSERT(lob.add_order(3, 1000, 60, Side::BUY, OrderType::LIMIT, 42), "P42 aggressive sweep");

    TEST_ASSERT(lob.best_ask() == MAX_PRICE_TICKS, "All asks cleared");
    TEST_ASSERT(lob.best_bid() == 1000, "Order 3 rests at 1000");
    TEST_ASSERT(lob.get_volume_at(Side::BUY, 1000) == 10, "Remaining 10 rests");

    OutboundEvent ev{};
    TEST_ASSERT(queue.pop(ev) && ev.type == EventType::ACCEPTED && ev.order_id == 3, "Order 3 accepted");
    TEST_ASSERT(queue.pop(ev) && ev.type == EventType::CANCELED && ev.order_id == 1 &&
                ev.resting_order_id == 3 && ev.quantity == 30 &&
                ev.reason == static_cast<uint8_t>(CancelReason::SELF_TRADE_PREVENTION),
                "Order 1 canceled via STP");
    TEST_ASSERT(queue.pop(ev) && ev.type == EventType::EXECUTED && ev.order_id == 3 &&
                ev.resting_order_id == 2 && ev.quantity == 50,
                "Order 3 matched against P99 Order 2");
    TEST_ASSERT(queue.pop(ev) && ev.type == EventType::RESTED && ev.order_id == 3 && ev.quantity == 10,
                "Order 3 rested remainder");
    TEST_ASSERT(queue.empty(), "All STP events verified");

    TEST_ASSERT(lob.verify_invariants(), "Invariants after STP test");
    std::cout << "[PASS] test_self_trade_prevention\n";
}

void test_spsc_has_capacity() {
    SPSCQueue<OutboundEvent, 8> small_q; // Usable capacity = 7 slots
    TEST_ASSERT(small_q.has_capacity(7), "Empty 8-slot queue has capacity for 7 items");
    TEST_ASSERT(!small_q.has_capacity(8), "8-slot ring buffer never has 8 usable slots");

    OutboundEvent dummy{};
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT(small_q.push(dummy), "Push dummy");
    }
    TEST_ASSERT(small_q.has_capacity(2), "2 slots remaining");
    TEST_ASSERT(!small_q.has_capacity(3), "Not enough space for 3");

    TEST_ASSERT(small_q.pop(dummy), "Pop 1 item");
    TEST_ASSERT(small_q.has_capacity(3), "Cached head refreshed on demand: now 3 slots free");

    std::cout << "[PASS] test_spsc_has_capacity\n";
}

int main() {
    std::cout << "Running Complete HFT Engine v2 Verification Suite...\n\n";
    
    test_startup_and_bounds();
    test_duplicate_order_id_rejection();
    test_bitmask_hierarchy_boundaries();
    test_linked_list_unlinking();
    test_reduce_order_and_fifo_priority();
    test_multi_level_sweep_and_rest();
    test_pool_and_hashmap_churn();
    
    test_post_only_orders();
    test_ioc_orders();
    test_fok_with_stp();
    test_self_trade_prevention();
    test_spsc_has_capacity();

    std::cout << "\nAll 12 test suites and deep invariant checks PASSED!\n";
    return 0;
}