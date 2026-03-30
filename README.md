# ApexLOB - Ultra-Low-Latency Limit Order Book

A high-performance C++20 limit order book engine designed for high-frequency trading applications, targeting sub-50 microsecond order-to-trade latency.

## Features

- **Lock-free SPSC ring buffers** — zero-contention inter-thread communication using acquire/release memory ordering
- **Object pooling** — zero heap-allocation on the critical path; all 200,000 orders pre-allocated at startup
- **Cache-line aligned data structures** — 64-byte `alignas` on every hot struct prevents false sharing across cores
- **Price-time priority matching** — standard exchange algorithm: best price first, FIFO within a price level
- **All four order types** — Limit (rest in book), Market (match at any price), IOC (cancel unfilled remainder), FOK (fill completely or cancel immediately via pre-scan)
- **Self-match prevention** — orders from the same `client_id` are skipped during matching
- **Cancel routing** — `CancelOrderMessage` is forwarded from the network thread to the matching thread via a dedicated lock-free cancel queue; cancels are drained with higher priority than new orders
- **Full TCP response path** — the output thread encodes `ExecutionReportMessage` and `TradeMessage` structs into framed binary messages and writes them back to the client socket
- **Graceful shutdown** — SIGINT/SIGTERM sets a shared atomic flag; all three threads exit their loops and join cleanly
- **Network byte order wire protocol** — fixed-size binary messages with 4-byte length-prefix framing; custom `hton<T>`/`ntoh<T>` templates handle endianness for 2/4/8-byte fields
- **Thread affinity** (Linux) — network, matching, and output threads pinned to CPU cores 0/1/2 to keep working sets in per-core caches

## Architecture

Three dedicated threads communicate through lock-free queues. Each thread has exactly one responsibility, eliminating contention:

```
TCP Client
    │
    ▼
Network Thread (Core 0)
    │  Parse framed binary messages
    │  Acquire Order* from ObjectPool
    │  Push Order* ──────────────────────► SPSCRingBuffer<Order*, 8192>
    │  Push OrderId ─────────────────────► SPSCRingBuffer<OrderId, 1024>  (cancel queue)
    │  Set client_fd (atomic<int>)
    │
    ▼ (input queue)
Matching Thread (Core 1)
    │  Drain cancel queue (priority)
    │  Pop Order*, run price-time priority matching
    │  Push ExecutionReport ─────────────► SPSCRingBuffer<ExecutionReport, 16384>
    │  Push Trade ───────────────────────► SPSCRingBuffer<Trade, 8192>
    │  Return filled/cancelled orders to ObjectPool
    │
    ▼ (output + trade queues)
Output Thread (Core 2)
    │  Pop ExecutionReport → encode → send framed bytes over client_fd
    │  Pop Trade → encode → send framed bytes over client_fd
    ▼
TCP Client
```

The matching thread never allocates memory — it only acquires pre-allocated `Order` objects from the pool and releases them when orders are filled or cancelled.

## Building

### Prerequisites

- CMake 3.20 or higher
- C++20 compatible compiler (GCC 10+, Clang 12+)
- pthread (POSIX threads)

### Build Instructions

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Run all 36 tests
./build/apexlob_tests

# Run server on default port 8888
./build/apexlob

# Run on custom port
./build/apexlob 9999
```

### Build Options

| Option | Default | Description |
|---|---|---|
| `BUILD_TESTING` | ON | Fetch GoogleTest and build test binary |
| `CMAKE_BUILD_TYPE` | Release | Release (`-O3 -march=native -flto`) or Debug |
| `ENABLE_SANITIZERS` | OFF | Compile with AddressSanitizer + UBSan for leak/UB detection |

```bash
# Sanitizer build
cmake -B build_san -DENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build_san
./build_san/apexlob_tests
```

## Testing

36 tests across 4 test suites:

| Suite | Tests | Coverage |
|---|---|---|
| `OrderBookTest` | 9 | Price-time priority, add/remove, spread, mid-price |
| `ObjectPoolTest` | 7 | Acquire/release, capacity, PoolGuard RAII, reset on release |
| `SPSCRingBufferTest` | 10 | FIFO, wrap-around, full/empty, SPSC correctness |
| `MatchingEngineTest` | 10 | Limit fill, partial fill, spread (no match), IOC, FOK full, FOK cancel, Market order, self-match, pool leak (1000 pairs), cancel |

```bash
./build/apexlob_tests --gtest_verbose
```

## Wire Protocol

### Message Framing

All messages use 4-byte length-prefix framing over TCP:

```
[FrameHeader: 4 bytes length][Message body: N bytes]
```

### Message Types

| Message | Direction | Size | Description |
|---|---|---|---|
| `NewOrderMessage` | Client → Server | 40 bytes | Submit Limit/Market/IOC/FOK order |
| `CancelOrderMessage` | Client → Server | 24 bytes | Cancel a resting order by `order_id` |
| `ExecutionReportMessage` | Server → Client | 56 bytes | Order status update (New/PartiallyFilled/Filled/Cancelled/Rejected) |
| `TradeMessage` | Server → Client | 48 bytes | Trade notification with buy/sell order IDs, price, quantity |

See `include/WireProtocol.hpp` for field layouts and byte-order conversion.

## Order Types

| Type | Behavior |
|---|---|
| **Limit** | Rests in book at specified price; matches at that price or better |
| **Market** | Matches immediately at best available price regardless of price field; unfilled remainder is cancelled |
| **IOC** | Fills whatever quantity is available now at crossing prices; cancels any unfilled remainder immediately |
| **FOK** | Pre-scans the book before any fills — executes only if the entire quantity can be filled; otherwise cancels with zero fills |

## Performance Considerations

- **Zero-allocation hot path** — `ObjectPool::acquire()` is a single atomic CAS (~5 ns); no `malloc`/`new` during order processing
- **CPU affinity** (Linux) — threads pinned to dedicated cores; prevents cache migration between cores
- **False sharing prevention** — `head_` and `tail_` indices in SPSC buffer each occupy a full 64-byte cache line
- **TCP_NODELAY** — disables Nagle's algorithm; every packet sent immediately
- **Fixed-point prices** — `int64_t` scaled by 10,000 (4 decimal places); deterministic integer arithmetic, no floating-point rounding
- **RDTSC timestamps** — ~1 CPU cycle per call vs. ~20–100 cycles for `std::chrono`; used for price-time priority ordering
- **Intrusive linked lists** — `Order` objects contain their own `prev_`/`next_` pointers; adding an order to a price level requires zero allocations

## Known Limitations

- **Price level lookup is O(n)** — `OrderBook` uses linear search through a fixed `std::array<PriceLevel, 10000>`. Production systems use sorted arrays with binary search or tick-indexed arrays for O(1) access.
- **Single client connection** — the server handles one TCP client at a time. Adding `epoll` and a `client_id → fd` map would support multiple simultaneous clients.
- **No persistence** — all order state is in-memory. A process crash loses the entire book. Production systems write a write-ahead log before processing each order.
- **Single symbol per engine** — `main.cpp` instantiates one `MatchingEngine` for symbol 1. The `MultiSymbolMatchingEngine` class exists and routes by symbol; it just needs to be wired into `main.cpp`.
- **No market data snapshots** — only incremental execution reports and trade messages are generated. Connecting clients cannot bootstrap their local book state without an L2 snapshot.
- **Thread affinity is Linux-only** — `pthread_setaffinity_np` is not available on macOS; the call is compiled out with `#ifdef __linux__`.
