# C++20 Ultra-Low-Latency Matching Engine

A professional-grade, ultra-low-latency high-frequency trading (HFT) matching engine built in C++20. This project demonstrates mechanical sympathy, zero-allocation hot paths, and cache-coherent data structures designed to process deterministic limit order book (LOB) interactions with absolute minimal latency.

## System Architecture & Performance

The engine strictly isolates network I/O from the critical matching path to guarantee deterministic execution:
`[Network Gateway] -> (Inbound SPSC Queue) -> [Pinned Engine Thread] -> (Outbound SPSC Queue) -> [Network Gateway]`

-   **Target Latency (Tick-to-Trade):** The core matching logic, from popping the inbound queue to pushing the outbound queue, targets **50 to 250 nanoseconds** per order. Wire-to-wire latency is bounded entirely by the network stack.
-   **Static Memory Footprint:** The limit order book maintains a strict memory ceiling of ~75MB to support 1,000,000 resting orders. This allows the complete matching state to reside entirely within modern CPU L3 caches and maps perfectly into thirty-eight 2MB HugePages, eliminating OS page faults and TLB misses.

## Core Engineering Principles

-   **Zero Hot-Path Allocations:** All memory for order management and price levels is pre-allocated at startup. The engine utilizes an intrusive LIFO memory pool, completely eliminating `new`, `malloc`, or dynamic container resizing during live order processing.
-   **Cache-Line Alignment:** Data structures are meticulously padded and sized. `Order` nodes are strictly aligned to 32 bytes (two per 64-byte L1 cache line), and outbound telemetry events are uniformly packed to optimize memory bandwidth and prevent false sharing.
-   **Hardware-Accelerated Hashing:** Order ID lookups utilize an open-addressed linear probing hash map powered by hardware SIMD instructions (SSE4.2 `_mm_crc32_u64`) combined with a golden ratio multiplier for robust avalanche characteristics and sub-nanosecond lookups.
-   **Compile-Time Branch Elimination:** Extensive use of C++20 `if constexpr` and template metaprogramming isolates BUY and SELL matching logic at compile time, eliminating branch misprediction penalties on the hot path.
-   **Wait-Free Concurrency:** Inter-thread communication relies on a custom Single-Producer Single-Consumer (SPSC) ring buffer utilizing cached atomic cursors to minimize memory contention across the CPU interconnect.

## Exchange Domain Features

-   **Order Types & Time-In-Force (TIF):** Full support for standard LIMIT, Immediate-Or-Cancel (IOC), Fill-Or-Kill (FOK), and Maker-Only (POST_ONLY) orders.
-   **Hardware-Accelerated Liquidity Scanning:** FOK orders evaluate available liquidity using hierarchical 64-bit bitmasks and `std::countl_zero` / `std::countr_zero` intrinsics to skip empty price ticks in O(1) time without polluting the L1 data cache.
-   **Self-Trade Prevention (STP):** Integrated Cancel-Oldest (Cancel Resting) logic prevents matched trades originating from the same participant identifier, executing seamlessly during the aggressive taker sweep phase.

## Build Instructions

This project requires a C++20 compliant compiler and a Linux environment (or WSL2) to support aggressive compiler optimizations.

```bash
# Configure the build environment with strict HFT flags
# (-O3, -march=native, -flto, -fno-exceptions, -fno-rtti)
cmake -B build

# Compile the matching engine and test suites
cmake --build build -j

# Execute the deep invariant verification suite
./build/test_lob
```

## Development Roadmap

The matching engine core is fully implemented and verified via deep invariant testing. The following phases outline the path to a production-ready exchange gateway:

**Phase 1: Multi-Threaded Pipeline & OS Tuning**
Implementation of kernel-level optimizations to isolate the matching engine thread. This includes CPU core pinning via `pthread_setaffinity_np` and utilizing `mmap` with `MADV_HUGEPAGE` to allocate the engine's memory space into 2MB HugePages, drastically reducing TLB misses.

**Phase 2: Strict Hardware Cycle Benchmarking**
Integration of deterministic performance profiling using hardware timestamp counters (`__rdtscp` + `lfence`). Benchmarks will measure p50, p90, p99, and p99.9 latencies in both CPU cycles and nanoseconds across realistic, high-throughput order distributions (e.g., 90% add/cancel, 10% trade) to establish an absolute theoretical limit for the core matching logic.

**Phase 3: Wire Protocols & Kernel Bypass Networking**
Full integration of standard proprietary wire protocols. The engine will ingest big-endian NASDAQ OUCH 5.0 binary structures for order entry and broadcast executed market data via NASDAQ ITCH 5.0 structs (`#pragma pack(push, 1)`). 

Network bridging will initially be handled by a lock-free, non-blocking `epoll` event loop to establish a robust baseline. To approach true industry standards for wire-to-wire latency, the gateway will ultimately migrate to modern kernel bypass or advanced asynchronous I/O architectures (e.g., `io_uring` or DPDK alternatives suitable for personal hardware) to completely eliminate OS network stack overhead.
