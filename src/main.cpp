#include <iostream>
#include <memory>
#include "engine/LimitOrderBook.h"

/// @brief Entry point demonstrating the initialization and basic operation of the HFT matching engine.
int main() {
    // Allocate the outbound telemetry queue and the Limit Order Book on the heap.
    // The LOB is ~75MB and must never be allocated on the thread stack.
    auto queue = std::make_unique<OutboundQueue>();
    auto lob = std::make_unique<LimitOrderBook>(queue.get());

    // Submit a resting LIMIT order (Maker)
    lob->add_order<Side::SELL>(1, 10050, 100, OrderType::LIMIT, 10);
    
    // Submit an aggressive IOC order (Taker)
    lob->add_order<Side::BUY>(2, 10050, 40, OrderType::IOC, 20);

    std::cout << "HFT Matching Engine v2 initialized.\n";
    std::cout << "Best Ask: " << lob->best_ask()
              << " | Remaining Ask Vol: " << lob->get_volume_at(Side::SELL, 10050) << "\n";

    // Drain and process the outbound event telemetry stream
    OutboundEvent ev{};
    while (queue->pop(ev)) {
        std::cout << "Event Type: " << static_cast<int>(ev.type)
                  << " | OrderID: " << ev.order_id
                  << " | Price: " << ev.price
                  << " | Qty: " << ev.quantity << "\n";
    }
    
    return 0;
}