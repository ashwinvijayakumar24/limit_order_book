# Requirements Document: ApexLOB

## Introduction

ApexLOB (Apex Limit Order Book) is an ultra-low-latency limit order book and matching engine designed for high-frequency trading environments. The system must achieve sub-50 microsecond execution latency on standard consumer CPUs through aggressive optimization of memory management, cache locality, and lock-free concurrency patterns. The implementation uses C++20, custom binary protocols over raw TCP sockets, and zero-allocation hot paths.

## Glossary

- **LOB**: Limit Order Book - a data structure maintaining buy and sell orders sorted by price-time priority
- **Matching_Engine**: The core component that matches buy orders against sell orders according to price-time priority rules
- **Hot_Path**: The critical execution path during order matching where no dynamic memory allocation is permitted
- **Object_Pool**: A pre-allocated memory arena that recycles fixed-size objects to eliminate allocation overhead
- **Ring_Buffer**: A lock-free circular buffer used for inter-thread communication
- **Wire_Protocol**: The custom binary message format used for network communication
- **Price_Level**: A specific price point in the order book containing one or more orders
- **Order**: A request to buy or sell a specific quantity at a specific price
- **Trade**: The result of matching a buy order with a sell order
- **Network_Thread**: The thread responsible for receiving and parsing incoming TCP messages
- **Matching_Thread**: The thread responsible for processing orders and executing trades

## Requirements

### Requirement 1: Ultra-Low-Latency Order Processing

**User Story:** As a high-frequency trader, I want order-to-trade latency under 50 microseconds, so that I can compete effectively in latency-sensitive markets.

#### Acceptance Criteria

1. WHEN an order is submitted to the matching engine, THE Matching_Engine SHALL process it within 50 microseconds on a standard consumer CPU
2. WHILE the system is under load, THE Matching_Engine SHALL maintain consistent sub-50 microsecond latency for the 99th percentile
3. THE Matching_Engine SHALL NOT perform any dynamic memory allocations (malloc/new) on the hot path
4. THE System SHALL use cache-aligned data structures to maximize CPU cache hit rates
5. THE System SHALL minimize branch mispredictions in the hot path through predictable code patterns

### Requirement 2: Lock-Free Inter-Thread Communication

**User Story:** As a system architect, I want lock-free communication between network and matching threads, so that thread contention does not introduce latency spikes.

#### Acceptance Criteria

1. WHEN the Network_Thread receives a message, THE System SHALL transfer it to the Matching_Thread using a lock-free ring buffer
2. THE Ring_Buffer SHALL support single-producer single-consumer semantics without mutexes or condition variables
3. WHEN the ring buffer is full, THE Network_Thread SHALL apply backpressure by slowing TCP reads
4. THE Ring_Buffer SHALL use memory barriers and atomic operations to ensure correctness across CPU cores
5. THE System SHALL align the ring buffer to cache line boundaries to prevent false sharing

### Requirement 3: Custom Binary Wire Protocol

**User Story:** As a system designer, I want a compact binary protocol over raw TCP sockets, so that serialization overhead is minimized compared to text-based protocols.

#### Acceptance Criteria

1. THE Wire_Protocol SHALL define message types for Add Order, Cancel Order, and Trade Execution
2. WHEN encoding an Add Order message, THE System SHALL use a fixed-size binary layout with explicit byte offsets
3. THE Wire_Protocol SHALL use network byte order (big-endian) for all multi-byte fields
4. WHEN parsing incoming messages, THE System SHALL validate message type and length before processing
5. THE Wire_Protocol SHALL include a timestamp field with nanosecond precision for latency measurement
6. THE System SHALL use raw BSD sockets with TCP_NODELAY enabled to minimize network latency

### Requirement 4: Zero-Allocation Memory Management

**User Story:** As a performance engineer, I want all order objects pre-allocated in a memory pool, so that the hot path never triggers OS memory allocation.

#### Acceptance Criteria

1. THE Object_Pool SHALL pre-allocate a fixed number of Order objects at system initialization
2. WHEN an order is needed, THE Object_Pool SHALL return a recycled object in O(1) time
3. WHEN an order is no longer needed, THE Object_Pool SHALL reclaim it for reuse without calling free()
4. THE Object_Pool SHALL align all objects to cache line boundaries to prevent false sharing
5. IF the object pool is exhausted, THEN THE System SHALL reject new orders with an error code
6. THE System SHALL use a separate Object_Pool for Trade objects with similar semantics

### Requirement 5: Price-Time Priority Order Book

**User Story:** As a trader, I want orders matched according to price-time priority rules, so that the system behaves like a standard exchange.

#### Acceptance Criteria

1. WHEN a buy order arrives, THE Order_Book SHALL match it against the best (lowest) sell price first
2. WHEN a sell order arrives, THE Order_Book SHALL match it against the best (highest) buy price first
3. WHILE multiple orders exist at the same price level, THE Order_Book SHALL match them in FIFO (first-in-first-out) order
4. WHEN an order is partially filled, THE Order_Book SHALL keep the remaining quantity in the book at the same priority
5. THE Order_Book SHALL maintain separate bid and ask sides with efficient price level lookup

### Requirement 6: Order Book Data Structure Optimization

**User Story:** As a systems architect, I want the order book to use cache-friendly data structures, so that price level lookups and iterations are fast.

#### Acceptance Criteria

1. THE Order_Book SHALL use a data structure that provides O(log N) or better price level lookup
2. THE Order_Book SHALL store orders at each price level in a contiguous memory layout for cache efficiency
3. WHEN iterating through price levels, THE System SHALL access memory sequentially to maximize cache prefetching
4. THE Order_Book SHALL use fixed-size arrays for price levels when the price range is bounded and known
5. THE System SHALL avoid pointer chasing and linked list traversals in the hot path

### Requirement 7: Order Lifecycle Management

**User Story:** As a trader, I want to add, cancel, and modify orders, so that I can manage my trading strategy.

#### Acceptance Criteria

1. WHEN a valid Add Order message is received, THE Matching_Engine SHALL attempt to match it immediately
2. IF an order cannot be fully matched, THEN THE Matching_Engine SHALL insert the remaining quantity into the Order_Book
3. WHEN a Cancel Order message is received, THE Matching_Engine SHALL remove the specified order from the Order_Book
4. IF a cancel request references a non-existent order, THEN THE System SHALL return an error response
5. WHEN an order is matched, THE System SHALL generate Trade messages for both the aggressor and resting orders

### Requirement 8: Comprehensive Testing Strategy

**User Story:** As a developer, I want unit tests and benchmarks, so that I can verify correctness and measure performance.

#### Acceptance Criteria

1. THE System SHALL use Google Test (GTest) for all unit tests
2. THE System SHALL use Google Benchmark for latency measurements
3. WHEN running benchmarks, THE System SHALL report p50, p99, and p999 latency percentiles
4. THE System SHALL include property-based tests for order matching correctness
5. THE System SHALL include integration tests that verify the full network-to-matching pipeline

### Requirement 9: Build System and Project Structure

**User Story:** As a developer, I want a clean CMake build system, so that the project is easy to build and maintain.

#### Acceptance Criteria

1. THE System SHALL use CMake as the build system with C++20 standard enabled
2. THE Project SHALL separate source files (src/), headers (include/), tests (tests/), and benchmarks (benchmarks/)
3. THE Build_System SHALL compile with -O3 optimization and appropriate warning flags (-Wall -Wextra)
4. THE Build_System SHALL support building unit tests and benchmarks as separate targets
5. THE Project SHALL include a top-level CMakeLists.txt that orchestrates all sub-projects

### Requirement 10: Thread Architecture and Separation

**User Story:** As a system architect, I want clear separation between network I/O and order matching, so that network latency does not block the matching engine.

#### Acceptance Criteria

1. THE System SHALL run the Network_Thread and Matching_Thread on separate CPU cores
2. THE Network_Thread SHALL be responsible for accepting connections, receiving messages, and parsing the wire protocol
3. THE Matching_Thread SHALL be responsible for order matching, trade generation, and order book updates
4. WHEN the Matching_Thread generates a trade, THE System SHALL send the trade message back through the Network_Thread
5. THE System SHALL use CPU affinity to pin threads to specific cores for consistent performance

### Requirement 11: Error Handling and Robustness

**User Story:** As a system operator, I want graceful error handling, so that invalid messages do not crash the system.

#### Acceptance Criteria

1. WHEN an invalid message is received, THE System SHALL log the error and continue processing
2. IF a TCP connection is closed unexpectedly, THEN THE Network_Thread SHALL clean up resources and accept new connections
3. WHEN the object pool is exhausted, THE System SHALL reject new orders with a specific error code
4. THE System SHALL validate all message fields (price > 0, quantity > 0) before processing
5. THE System SHALL use RAII patterns to ensure resource cleanup in error paths

### Requirement 12: Performance Measurement and Observability

**User Story:** As a performance engineer, I want detailed latency metrics, so that I can identify bottlenecks.

#### Acceptance Criteria

1. THE System SHALL measure end-to-end latency from message receipt to trade execution
2. THE System SHALL use high-resolution timestamps (std::chrono::high_resolution_clock or RDTSC)
3. WHEN running benchmarks, THE System SHALL report latency histograms with microsecond granularity
4. THE System SHALL include a benchmark that measures object pool allocation/deallocation overhead
5. THE System SHALL include a benchmark that measures ring buffer throughput and latency
