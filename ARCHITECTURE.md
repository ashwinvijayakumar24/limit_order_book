# ApexLOB Architecture Overview

**Ultra-low-latency C++20 Limit Order Book for High-Frequency Trading**

Target: <50 microsecond order-to-trade latency

---

## System Architecture

```
┌─────────────────┐         ┌──────────────────┐         ┌─────────────────┐
│  Network Thread │────────▶│  SPSC RingBuffer │────────▶│ Matching Thread │
│  (WireProtocol) │         │  (lock-free)     │         │ (MatchingEngine)│
└─────────────────┘         └──────────────────┘         └─────────────────┘
                                                                    │
                                                                    ▼
                                                          ┌─────────────────┐
                                                          │   OrderBook     │
                                                          │  (price-time)   │
                                                          └─────────────────┘
                                                                    │
                                                                    ▼
                                                          ┌─────────────────┐
                                                          │  ObjectPool     │
                                                          │  (Order cache)  │
                                                          └─────────────────┘
```

---

## Component Descriptions

### 1. **Types.hpp** - Core Type Definitions
- **Fixed-point Price**: `int64_t` with 4 decimal places (avoids floating-point non-determinism)
- **Enumerations**: Side (Buy/Sell), OrderType (Limit/Market/IOC/FOK), ExecutionStatus
- **Cache alignment constants**: 64-byte cache line sizing
- **C++20 concepts**: `TriviallyCopyable`, `CacheAligned`, `Poolable`
- **RDTSC timestamp**: Sub-microsecond timing for x86-64

### 2. **Order.hpp** - Cache-Optimized Order Structure
```
┌─────────────── Cache Line 1 (HOT DATA) ────────────────┐
│ OrderId | Price | Quantity | Filled | Side | Type     │
│ prev* | next* | entry_timestamp                       │
└────────────────────────────────────────────────────────┘
┌─────────────── Cache Line 2 (COLD DATA) ───────────────┐
│ ClientId | Symbol | LastModified | ClientOrderId      │
└────────────────────────────────────────────────────────┘
```
- **Size**: Exactly 128 bytes (2 cache lines)
- **Intrusive list pointers**: `prev_`, `next_` for zero-allocation book management
- **Hot/cold separation**: Frequently accessed fields in first cache line
- **Price-time priority**: Comparators for buy (highest price) vs sell (lowest price)

### 3. **ObjectPool.hpp** - Lock-Free Memory Pool
- **Template-based**: `ObjectPool<T, Capacity>` with compile-time sizing
- **O(1) acquire/release**: Atomic CAS operations on intrusive free-list
- **Cache-line aligned**: Each object aligned to 64 bytes
- **SPSC optimized**: Memory ordering (`acquire`/`release`) for single producer/consumer
- **RAII guard**: `PoolGuard<T>` for automatic cleanup
- **Zero allocations**: All memory pre-allocated at construction

### 4. **SPSCRingBuffer.hpp** - Lock-Free Queue
- **Power-of-two sizing**: Fast modulo via bitwise AND
- **False sharing prevention**: Head/tail atomics on separate cache lines
- **Memory ordering**: `memory_order_acquire`/`release` for cross-thread visibility
- **Variants**: `SPSCRingBuffer` (single producer) and `MPSCRingBuffer` (multi-producer)
- **Batch operations**: `SPSCBatchOps` for higher throughput
- **Compile-time validation**: Requires `is_power_of_two(Capacity)`

### 5. **OrderBook.hpp** - Price-Time Priority Matching
```
OrderBook
├── BuyBookSide (highest price first)
│   ├── PriceLevel[0]: $100.05 → [Order1] → [Order2] → ...
│   ├── PriceLevel[1]: $100.04 → [Order3] → ...
│   └── ...
└── SellBookSide (lowest price first)
    ├── PriceLevel[0]: $100.06 → [Order4] → [Order5] → ...
    └── ...
```
- **PriceLevel**: Intrusive doubly-linked list of orders at same price
- **Fixed-size arrays**: `std::array<PriceLevel, MAX_PRICE_LEVELS>`
- **Best bid/ask tracking**: O(1) access to top of book
- **Aggregate depth**: Total quantity per price level
- **Virtual dispatch**: Buy/sell sides override `is_better_price()` for comparison logic

**Note**: Current implementation uses linear search for price levels. Production systems should use:
- Sorted arrays with binary search
- Skip lists or B-trees for O(log n) insertion
- Hash maps with price quantization

### 6. **MatchingEngine.hpp** - Core Orchestration
```cpp
process_order(Order*) {
    1. Validate order
    2. Assign order ID
    3. match_order() - cross with opposite side
    4. Add unfilled portion to OrderBook
    5. Send ExecutionReport
}

match_order() {
    while (can_match && !fully_filled) {
        1. Check price crossing
        2. Check self-match prevention
        3. execute_trade() - fill orders
        4. Update book quantities
        5. Remove fully filled orders
    }
}
```
- **Template parameters**: Compile-time sizing for pools and queues
- **Order map**: `unordered_map<OrderId, Order*>` for O(1) lookups
- **Trade generation**: Populates `TradeQueue` with execution details
- **Execution reports**: Sent to `OutputQueue` for each order state change
- **Multi-symbol support**: `MultiSymbolMatchingEngine` manages per-symbol books

### 7. **WireProtocol.hpp** - Binary Message Encoding
- **Network byte order**: `hton()`/`ntoh()` for big-endian conversion
- **Fixed-size messages**: No variable-length fields (predictable parsing)
- **Message types**:
  - `NewOrderMessage` (40 bytes)
  - `CancelOrderMessage` (24 bytes)
  - `ExecutionReportMessage` (56 bytes)
  - `TradeMessage` (48 bytes)
- **Zero-copy parsing**: `std::memcpy` + in-place byte swapping
- **Framing support**: Length-prefixed messages for TCP streams

---

## Memory Layout Optimizations

### Cache Line Alignment (64 bytes)
```cpp
struct alignas(64) Order { ... };         // 128 bytes (2 lines)
struct alignas(64) Trade { ... };         // 64 bytes (1 line)
struct alignas(64) PriceLevel { ... };    // 64 bytes (1 line)
```

### False Sharing Prevention
```cpp
alignas(64) std::atomic<size_t> head_;    // Own cache line
alignas(64) std::atomic<size_t> tail_;    // Own cache line
```

---

## C++20 Feature Usage

### Concepts
```cpp
template<typename T>
concept Poolable = requires(T t) {
    { t.next_ } -> std::convertible_to<T*>;
    std::is_trivially_destructible_v<T>;
};

template<typename T, size_t N>
    requires TriviallyCopyable<T> && is_power_of_two(N)
class SPSCRingBuffer { ... };
```

### Three-Way Comparison
```cpp
constexpr auto operator<=>(const Price&) const noexcept = default;
```

### Constexpr Everything
- All core types are fully `constexpr` for compile-time validation
- Enables compile-time unit testing of price math

### std::span
```cpp
bool parse_header(std::span<const std::byte> buffer, MessageHeader& header);
```

---

## Threading Model

### Recommended Architecture
```
Thread 1 (Network I/O)          Thread 2 (Matching)
      │                               │
      ├─ Parse wire messages         ├─ Pop from InputQueue
      ├─ Acquire from ObjectPool     ├─ Match orders
      ├─ Push to InputQueue          ├─ Update OrderBook
      │                               ├─ Push to OutputQueue
      │                               └─ Push to TradeQueue
      │
Thread 3 (Reporting)
      │
      ├─ Pop from OutputQueue
      ├─ Encode wire messages
      └─ Send to clients
```

**Critical**: Network and matching threads NEVER share writable data structures. Communication only via lock-free queues.

---

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Add order to pool | O(1) | Atomic CAS on free-list |
| Add order to book | O(n) | Linear search for price level* |
| Match order | O(k) | k = orders matched |
| Cancel order | O(1) | Direct pointer via order map |
| Push to queue | O(1) | Lock-free atomic increment |
| Best bid/ask | O(1) | Cached pointer |

*Replace with O(log n) sorted structure in production

---

## Size Constraints

```cpp
constexpr size_t MAX_ORDERS_PER_SIDE = 100'000;
constexpr size_t MAX_PRICE_LEVELS = 10'000;

// Memory usage estimate:
// - ObjectPool: 100k orders × 128 bytes = 12.8 MB
// - OrderBook: 10k levels × 64 bytes = 640 KB
// - Total per symbol: ~13.5 MB
```

---

## Usage Example (Pseudocode)

```cpp
// Setup
MatchingEngine<> engine(symbol_id);
SPSCRingBuffer<Order*, 8192> input_queue;
SPSCRingBuffer<ExecutionReport, 16384> output_queue;
SPSCRingBuffer<Trade, 8192> trade_queue;

// Network thread: receive new order
NewOrderMessage msg = parse_from_network(buffer);
Order* order = engine.order_pool().acquire();
*order = msg.to_order();
input_queue.try_push(order);

// Matching thread: process orders
engine.process_inbound_orders(input_queue, output_queue, trade_queue);

// Reporting thread: send executions
if (auto report = output_queue.try_pop()) {
    auto msg = ExecutionReportMessage::from_report(*report, seq++);
    send_to_network(msg);
}
```

---

## Next Steps for Implementation

1. **Write implementations** (.cpp files) for each header
2. **Add logging** (structured binary logs for replay)
3. **Upgrade price level storage** (sorted array or skip list)
4. **Add benchmarks** (measure latency with rdtsc)
5. **Implement market data** (L2 book snapshots, incremental updates)
6. **Add persistence** (crash recovery, journal)
7. **CPU pinning** (isolate matching thread, disable hyperthreading)
8. **NUMA awareness** (bind memory to CPU socket)

---

## Compile Flags (Recommended)

```bash
g++ -std=c++20 -O3 -march=native -flto \
    -DNDEBUG -fno-exceptions -fno-rtti \
    -Wall -Wextra -Wpedantic
```

- **`-O3`**: Maximum optimization
- **`-march=native`**: Use CPU-specific instructions (AVX, etc.)
- **`-flto`**: Link-time optimization
- **`-fno-exceptions`**: Disable exceptions (latency sensitive)
- **`-fno-rtti`**: Disable runtime type info (smaller code)
