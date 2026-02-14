# Implementation Plan: ApexLOB

## Overview

This implementation plan breaks down the ApexLOB ultra-low-latency limit order book into four distinct phases: MVP Core, Performance Optimization, Advanced Features, and Production Hardening. Each phase builds incrementally on the previous one, allowing for early validation of core functionality and performance characteristics.

The implementation prioritizes the hot path (order matching) first, then adds networking and protocol layers, followed by optimization and production features. Property-based tests are integrated throughout to validate correctness properties from the design document.

## Tasks

- [ ] 1. Project Setup and Core Infrastructure
  - Create CMake build system with C++20 support and optimization flags
  - Set up directory structure (src/, include/, tests/, benchmarks/)
  - Configure Google Test and Google Benchmark integration
  - Create basic type definitions and constants
  - _Requirements: 9.1, 9.2, 9.3, 9.4, 9.5_

- [ ]* 1.1 Write unit tests for project setup
  - Test that build system compiles with correct C++20 flags
  - Verify directory structure and include paths
  - _Requirements: 8.1, 8.2_

- [ ] 2. Object Pool Implementation
  - [ ] 2.1 Implement cache-aligned ObjectPool template class
    - Create fixed-size pre-allocated object pool with O(1) acquire/release
    - Implement cache-line alignment and RAII semantics
    - _Requirements: 4.1, 4.2, 4.3, 4.4_

  - [ ]* 2.2 Write property test for object pool constant time access
    - **Property 10: Object Pool Constant Time Access**
    - **Validates: Requirements 4.2**

  - [ ]* 2.3 Write property test for zero-allocation recycling
    - **Property 11: Zero-Allocation Object Recycling**
    - **Validates: Requirements 4.3**

  - [ ]* 2.4 Write unit tests for object pool edge cases
    - Test pool exhaustion handling and error conditions
    - _Requirements: 4.5, 11.3_

- [ ] 3. Lock-Free Ring Buffer Implementation
  - [ ] 3.1 Implement SPSC ring buffer with atomic operations
    - Create single-producer single-consumer ring buffer using C++20 atomics
    - Implement cache-line alignment and memory barriers
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5_

  - [ ]* 3.2 Write property test for lock-free message transfer
    - **Property 4: Lock-Free Message Transfer**
    - **Validates: Requirements 2.1**

  - [ ]* 3.3 Write property test for concurrent access correctness
    - **Property 6: Concurrent Access Correctness**
    - **Validates: Requirements 2.4**

  - [ ]* 3.4 Write unit tests for ring buffer boundary conditions
    - Test full/empty conditions and wrap-around behavior
    - _Requirements: 2.3_

- [ ] 4. Wire Protocol Implementation
  - [ ] 4.1 Implement binary message parsing and encoding
    - Create WireProtocol class with fixed-size message handling
    - Implement network byte order conversion functions
    - Add message validation for type and length
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6_

  - [ ]* 4.2 Write property test for wire protocol round trip
    - **Property 7: Wire Protocol Round Trip**
    - **Validates: Requirements 3.2**

  - [ ]* 4.3 Write property test for network byte order consistency
    - **Property 8: Network Byte Order Consistency**
    - **Validates: Requirements 3.3**

  - [ ]* 4.4 Write property test for message validation
    - **Property 9: Message Validation**
    - **Validates: Requirements 3.4**

  - [ ]* 4.5 Write unit tests for protocol error handling
    - Test invalid message types and malformed messages
    - _Requirements: 11.4_

- [ ] 5. Order Book Data Structure
  - [ ] 5.1 Implement cache-optimized OrderBook class
    - Create fixed-size array-based price levels for known price ranges
    - Implement efficient price-to-index mapping
    - Add best bid/ask tracking with atomic operations
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

  - [ ]* 5.2 Write property test for price priority matching (buy orders)
    - **Property 12: Price Priority Matching (Buy Orders)**
    - **Validates: Requirements 5.1**

  - [ ]* 5.3 Write property test for price priority matching (sell orders)
    - **Property 13: Price Priority Matching (Sell Orders)**
    - **Validates: Requirements 5.2**

  - [ ]* 5.4 Write property test for time priority within price levels
    - **Property 14: Time Priority Within Price Levels**
    - **Validates: Requirements 5.3**

  - [ ]* 5.5 Write property test for efficient price level lookup
    - **Property 16: Efficient Price Level Lookup**
    - **Validates: Requirements 6.1**

- [ ] 6. Checkpoint - Core Data Structures Complete
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 7. Matching Engine Core Logic
  - [ ] 7.1 Implement MatchingEngine class with order processing
    - Create main matching loop with immediate order processing
    - Implement order-to-trade matching logic with price-time priority
    - Add partial fill handling and order book insertion
    - _Requirements: 7.1, 7.2, 7.4, 7.5_

  - [ ]* 7.2 Write property test for immediate matching attempt
    - **Property 17: Immediate Matching Attempt**
    - **Validates: Requirements 7.1**

  - [ ]* 7.3 Write property test for unmatched quantity insertion
    - **Property 18: Unmatched Quantity Insertion**
    - **Validates: Requirements 7.2**

  - [ ]* 7.4 Write property test for partial fill preservation
    - **Property 15: Partial Fill Preservation**
    - **Validates: Requirements 5.4**

  - [ ]* 7.5 Write property test for trade message generation
    - **Property 20: Trade Message Generation**
    - **Validates: Requirements 7.5**

- [ ] 8. Order Cancellation Logic
  - [ ] 8.1 Implement order cancellation functionality
    - Add order ID to order mapping for fast cancellation lookup
    - Implement cancel order processing with error handling
    - _Requirements: 7.3, 7.4_

  - [ ]* 8.2 Write property test for order cancellation
    - **Property 19: Order Cancellation**
    - **Validates: Requirements 7.3**

  - [ ]* 8.3 Write unit tests for cancellation error cases
    - Test non-existent order ID and already-filled orders
    - _Requirements: 11.2_

- [ ] 9. TCP Network Layer
  - [ ] 9.1 Implement TCPServer class with raw BSD sockets
    - Create TCP server with connection handling and socket configuration
    - Implement non-blocking I/O with proper error handling
    - Add TCP_NODELAY and buffer size optimization
    - _Requirements: 3.6, 10.1, 10.2, 10.4, 11.2_

  - [ ]* 9.2 Write unit tests for network connection handling
    - Test connection acceptance, disconnection, and error recovery
    - _Requirements: 11.2_

- [ ] 10. Thread Architecture Integration
  - [ ] 10.1 Implement dual-thread architecture
    - Create Network_Thread for TCP handling and message parsing
    - Create Matching_Thread for order processing and matching
    - Integrate ring buffer communication between threads
    - _Requirements: 10.1, 10.2, 10.3, 10.5_

  - [ ]* 10.2 Write integration tests for thread communication
    - Test end-to-end message flow from network to matching thread
    - _Requirements: 2.1, 10.4_

- [ ] 11. Checkpoint - Basic System Integration Complete
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 12. Performance Optimization Phase
  - [ ] 12.1 Add CPU affinity and thread pinning
    - Pin Network_Thread to Core 0 and Matching_Thread to Core 1
    - Implement CPU affinity configuration
    - _Requirements: 10.5_

  - [ ] 12.2 Implement high-resolution latency measurement
    - Add timestamp collection using std::chrono::high_resolution_clock
    - Create latency tracking and reporting infrastructure
    - _Requirements: 12.1, 12.2_

  - [ ]* 12.3 Write property test for sub-50 microsecond processing latency
    - **Property 1: Sub-50 Microsecond Processing Latency**
    - **Validates: Requirements 1.1**

  - [ ]* 12.4 Write property test for consistent latency under load
    - **Property 2: Consistent Latency Under Load**
    - **Validates: Requirements 1.2**

  - [ ]* 12.5 Write property test for zero hot path allocations
    - **Property 3: Zero Hot Path Allocations**
    - **Validates: Requirements 1.3**

- [ ] 13. Error Handling and Robustness
  - [ ] 13.1 Implement comprehensive error handling
    - Add input validation for all message fields
    - Implement graceful handling of invalid messages
    - Add resource exhaustion handling
    - _Requirements: 11.1, 11.3, 11.4, 11.5_

  - [ ]* 13.2 Write property test for invalid message resilience
    - **Property 21: Invalid Message Resilience**
    - **Validates: Requirements 11.1**

  - [ ]* 13.3 Write property test for field validation
    - **Property 22: Field Validation**
    - **Validates: Requirements 11.4**

  - [ ]* 13.4 Write unit tests for error recovery scenarios
    - Test system behavior under various error conditions
    - _Requirements: 11.2, 11.3, 11.5_

- [ ] 14. Benchmarking and Performance Validation
  - [ ] 14.1 Implement comprehensive benchmarks
    - Create Google Benchmark tests for all critical components
    - Measure latency percentiles (p50, p99, p999) under various loads
    - Add throughput benchmarks for orders per second
    - _Requirements: 8.3, 12.3, 12.4, 12.5_

  - [ ]* 14.2 Write benchmark for object pool performance
    - Measure allocation/deallocation overhead
    - _Requirements: 12.5_

  - [ ]* 14.3 Write benchmark for ring buffer throughput
    - Measure enqueue/dequeue latency and throughput
    - _Requirements: 12.5_

- [ ] 15. Final Integration and System Testing
  - [ ] 15.1 Implement end-to-end integration tests
    - Test complete pipeline from TCP message to trade response
    - Add stress testing with sustained high-frequency order flow
    - Test various market scenarios (trending, volatile, quiet)
    - _Requirements: 8.5_

  - [ ]* 15.2 Write stress tests for resource exhaustion
    - Test system behavior when object pools are exhausted
    - Test ring buffer backpressure mechanisms
    - _Requirements: 11.3_

- [ ] 16. Final Checkpoint - Production Ready System
  - Ensure all tests pass, ask the user if questions arise.
  - Validate that all performance requirements are met
  - Confirm system stability under sustained load

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation at key milestones
- Property tests validate universal correctness properties from the design
- Unit tests validate specific examples and edge cases
- The implementation follows a bottom-up approach: core data structures → matching logic → networking → optimization
- Performance validation is integrated throughout rather than deferred to the end