# ApexLOB - Ultra-Low-Latency Limit Order Book

A high-performance C++20 limit order book system designed for high-frequency trading applications with sub-50 microsecond order-to-trade latency targets.

## Features

- **Lock-free SPSC ring buffers** for zero-contention inter-thread communication
- **Object pooling** for zero-allocation order processing
- **Cache-line aligned** data structures to prevent false sharing
- **Price-time priority** matching engine
- **TCP server** with thread affinity and TCP_NODELAY
- **Network byte order** wire protocol for cross-platform compatibility

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed system architecture and design decisions.

## Building

### Prerequisites

- CMake 3.20 or higher
- C++20 compatible compiler (GCC 10+, Clang 12+, MSVC 2019+)
- pthread (POSIX threads)

### Build Instructions

```bash
# Create build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Build
cmake --build . --config Release

# Run tests (optional)
ctest
```

### Build Options

- `BUILD_TESTING`: Enable Google Test integration (default: ON)
- `CMAKE_BUILD_TYPE`: Release (optimized) or Debug

## Running

```bash
# Run server on default port 8888
./apexlob

# Run on custom port
./apexlob 9999
```

The server will:
- Bind to the specified port
- Pin network thread to CPU core 0
- Pin matching thread to CPU core 1
- Pin output thread to CPU core 2

## Testing

```bash
# Build and run tests
cd build
cmake --build . --target apexlob_tests
./apexlob_tests
```

## Wire Protocol

### Message Format

All messages are length-prefixed with a `FrameHeader`:

```
[FrameHeader: 4 bytes][Message: variable]
```

### Supported Messages

- **NewOrderMessage** (40 bytes): Submit a new limit/market order
- **CancelOrderMessage** (24 bytes): Cancel an existing order
- **ExecutionReportMessage** (56 bytes): Order status updates
- **TradeMessage** (48 bytes): Trade notifications

See `include/WireProtocol.hpp` for message structures.

## Performance Considerations

- **CPU Affinity**: Threads are pinned to specific cores to reduce context switching
- **TCP_NODELAY**: Disables Nagle's algorithm for lower latency
- **Lock-free queues**: SPSC ring buffers eliminate mutex contention
- **Object pooling**: Pre-allocated orders prevent heap allocations in hot path
- **Cache alignment**: 64-byte alignment prevents false sharing

## Limitations

- Price level storage uses linear search (O(n)). Production systems should use sorted arrays or skip lists for O(log n) insertion.
- Single symbol per engine instance (use `MultiSymbolMatchingEngine` for multiple symbols)
- Basic TCP server (no connection pooling, authentication, etc.)

## License

[Specify your license here]
