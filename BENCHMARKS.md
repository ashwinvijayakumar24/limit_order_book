# ApexLOB Benchmarks

Every number below was produced by code in `bench/`, on the machine described
under [Test environment](#test-environment). Re-run instructions are at the
bottom. Nothing here is estimated or extrapolated.

## Test environment

| | |
|---|---|
| CPU | Apple Silicon, 10 cores |
| L1d / L1i / L2 | 64 KiB / 128 KiB / 4096 KiB (×10) |
| OS | macOS (Darwin 24.6.0), arm64 |
| Compiler | Apple Clang, C++20 |
| Flags | `-O3 -march=native -flto -DNDEBUG` (CMake `Release`) |
| Clock | `std::chrono::steady_clock`, measured resolution **~41 ns** |

**Important platform caveat.** `Types.hpp::rdtsc()` is implemented only for
x86-64; on arm64 it returns `0`. The engine's own `entry_timestamp` /
`MatchingEngineStats` latency counters are therefore unusable on this machine,
which is why the harness times everything externally with `steady_clock`.

All latencies are **in-process**. They exclude NIC, kernel TCP receive, and
network transit. Threads are not pinned in these runs (`pthread_setaffinity_np`
is Linux-only; `src/main.cpp` uses it, the benchmarks do not).

---

## 1. End-to-end latency and throughput

From `bench/latency_harness.cpp`. 200,000 measured samples, 10,000 warmup.
Workload is matched pairs: one resting sell (client 2), one crossing buy
(client 1) that fully fills it. Only the crossing order is measured.

### 1a. Engine only — wire decode + match, single thread

Timed region covers: `parse_new_order` → pool `acquire` → validate → match →
trade emitted.

| Percentile | Latency |
|---|---|
| min | 0 ns *(below clock resolution)* |
| mean | 69.7 ns |
| p50 | 83 ns |
| p90 | 84 ns |
| p99 | 125 ns |
| p99.9 | 208 ns |
| max | 34.9 µs |

Implied single-thread rate: **14.3 M matched orders/sec**.

Samples quantize to multiples of ~41 ns (the clock tick), so treat individual
percentiles below ~100 ns as ±1 tick. The `max` is OS scheduler noise, not
engine cost.

### 1b. Full pipeline, saturated — 3 threads, 2 SPSC hops

Producer decodes and enqueues flat out; matching thread drains and processes;
consumer thread drains execution reports. 420,000 orders processed.

| Metric | Value |
|---|---|
| Sustained rate | **5.15 M orders/sec** |
| Wall time | 0.082 s |
| p50 latency | 1.54 ms |
| p99 latency | 2.66 ms |

The millisecond latencies here are **queueing delay under saturation**, not
per-order cost — the producer outruns the matcher, so orders wait in the ring.
This run measures throughput capacity. For responsiveness, see 1c.

### 1c. Full pipeline, closed loop — one order in flight

Producer submits a pair, waits for the execution report, then submits the next.
50,000 samples. This is the honest end-to-end latency figure.

| Percentile | Latency |
|---|---|
| min | 291 ns |
| mean | 423 ns |
| p50 | 416 ns |
| p90 | 458 ns |
| p99 | 792 ns |
| p99.9 | 7.4 µs |
| max | 56.5 µs |

Covers decode + SPSC hop + match + SPSC hop + cross-core visibility.

---

## 2. Object pool vs. heap allocation

From `bench/bench_object_pool.cpp`. `Order` is 128 B, 64 B-aligned.

| Benchmark | Time/op | Rate |
|---|---|---|
| `PoolAcquireRelease` | **7.57 ns** | 132.6 M/s |
| `HeapNewDelete` | 57.0 ns | 18.6 M/s |
| `PoolBatchAcquireRelease/64` | 8.06 ns | 124.4 M/s |
| `PoolBatchAcquireRelease/1024` | 8.94 ns | 112.2 M/s |
| `PoolBatchAcquireRelease/16384` | 8.73 ns | 114.8 M/s |
| `HeapBatchNewDelete/64` | 46.8 ns | 21.5 M/s |
| `HeapBatchNewDelete/1024` | 46.2 ns | 21.7 M/s |
| `HeapBatchNewDelete/16384` | 67.8 ns | 14.8 M/s |

**Pool is 7.5× faster than `new`/`delete`** in the single-object case and
7.8× faster at 16 K batch depth. Pool cost is flat with depth; heap cost is not.

---

## 3. SPSC ring buffer vs. mutex-guarded queue

From `bench/bench_ring_buffer.cpp`. Baseline is `std::queue` + `std::mutex`.

### Single-threaded op cost (uncontended — best case for the mutex)

| Benchmark | Time per push+pop |
|---|---|
| `RingPushPop_SingleThread` | **2.09 ns** |
| `MutexQueuePushPop_SingleThread` | 8.74 ns |

**4.2× faster** even with zero contention.

### Cross-thread throughput (one producer thread, one consumer thread, 1 M msgs)

| Benchmark | Throughput |
|---|---|
| `RingThroughput_CrossThread_OrderPtr` | **35.9 M msgs/sec** |
| `RingThroughput_CrossThread_Trade` (64 B payload) | 35.1 M msgs/sec |
| `MutexQueueThroughput_CrossThread` | 7.60 M msgs/sec |

**4.7× higher throughput than the mutex queue** under real two-thread
contention. `Order*` and 64-byte `Trade` payloads perform within 2% of each
other, which is what cache-line-aligned head/tail (no false sharing) buys.

---

## 4. Wire protocol: binary vs. FIX-style text

From `bench/bench_wire_protocol.cpp`. The text baseline parses the *same seven
fields* from a SOH-delimited `tag=value` FIX `NewOrderSingle`, using
`std::from_chars` (faster than `atoi`/`strtod`, so the baseline is not a
strawman).

| Benchmark | Time | Rate |
|---|---|---|
| `BinaryEncodeNewOrder` | 0.255 ns | 3.94 G/s |
| `BinaryParseNewOrder` | 0.610 ns | 1.64 G/s |
| `BinaryParseNewOrderToOrder` (parse + build `Order`) | **2.39 ns** | 419 M/s |
| `TextFixParseNewOrder` | 74.3 ns | 13.5 M/s |

**31× faster** comparing the full network-thread path (parse + convert to
`Order`, 2.39 ns) against text parsing (74.3 ns).

Treat the sub-nanosecond encode/parse figures with care: at 0.26–0.61 ns the
compiler has visibility into the constant input and part of the work may be
hoisted despite `DoNotOptimize`. **The 31× figure from
`BinaryParseNewOrderToOrder` is the defensible one** — it is 4× above the
optimizer-noise floor and represents real work the network thread does.

---

## 5. Order book operations

From `bench/bench_order_book.cpp`.

`BookSide::find_or_create_level` uses a **linear scan** over price levels, so
insert cost is O(levels). The sweep below makes that explicit.

| Levels in book | Add+remove at existing level | Create new level |
|---|---|---|
| 1 | 3.32 ns | 5.17 ns |
| 10 | 20.4 ns | 27.1 ns |
| 100 | 172 ns | 221 ns |
| 1000 | 1579 ns | 2074 ns |

`best_bid()` lookup: **0.376 ns** (cached best-level index, O(1)).

This linear scan is the single clearest scaling limit in the codebase. At
realistic book depth (~100 levels) an insert costs 172 ns; at 1000 levels it
costs 1.6 µs. Replacing the scan with a sorted structure or a price-indexed
array is the highest-value optimization remaining.

---

## 6. Matching engine operations

From `bench/bench_matching_engine.cpp`. Manual timing (batches of 2048), since
`PauseTiming` overhead would dominate operations this small.

| Benchmark | Time |
|---|---|
| `ProcessOrder_RestNoMatch` (validate + insert, no cross) | **19.1 ns** |
| `ProcessOrder_FullMatchOneLevel` (one full fill + 2 reports) | **61.7 ns** |
| `CancelOrder` (hash lookup + unlink + report + release) | **24.6 ns** |

Sweeping N price levels with one aggressive order:

| Levels swept | Time per sweep | Time per fill |
|---|---|---|
| 1 | 62.3 ns | 62.3 ns |
| 5 | 193 ns | 38.6 ns |
| 20 | 829 ns | 41.5 ns |
| 100 | 7316 ns | 73.2 ns |

Per-fill cost stays roughly flat (~40–70 ns) as sweep depth grows; the rise at
100 levels reflects the O(levels) book operations from §5.

---

## 7. Memory safety

**Valgrind does not support Apple Silicon (arm64 macOS)** and was not run.
The equivalent evidence on this platform is AddressSanitizer + UndefinedBehavior
Sanitizer, which the build already supports via `-DENABLE_SANITIZERS=ON`:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan --target apexlob_tests
./build-asan/apexlob_tests
```

Result: **36 / 36 tests pass under ASan + UBSan**, no sanitizer diagnostics.

Test suite breakdown (36 total): `test_ring_buffer` 10, `test_matching_engine`
10, `test_order_book` 9, `test_object_pool` 7.

Note that LeakSanitizer is not enabled by default on macOS, so this run proves
absence of overflows, use-after-free, and UB — not absence of leaks. The
`PoolNotExhaustedAfterFills` test covers pool accounting directly instead.

---

## Reproducing

```bash
# Microbenchmarks (fetches Google Benchmark v1.8.3)
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON -DBUILD_TESTING=OFF
cmake --build build-bench -j

# Component microbenchmarks — run in groups; cross-thread ones are slow
./build-bench/apexlob_bench --benchmark_min_time=0.3s --benchmark_filter="Pool|Heap"
./build-bench/apexlob_bench --benchmark_min_time=0.3s --benchmark_filter="Binary|Text|OrderBook"
./build-bench/apexlob_bench --benchmark_min_time=0.3s --benchmark_filter="SingleThread"
./build-bench/apexlob_bench --benchmark_min_time=3x    --benchmark_filter="CrossThread"
./build-bench/apexlob_bench --benchmark_min_time=10x   --benchmark_filter="ProcessOrder|Cancel"

# End-to-end latency percentiles + throughput  (args: iterations, warmup)
./build-bench/apexlob_latency 200000 10000
```

Benchmarks that spawn threads or rebuild a 25 MB order pool per iteration need
`--benchmark_min_time` expressed in **iterations** (`10x`), not seconds, or
Google Benchmark will run them for a very long time.

## Known limitations of these measurements

- Single symbol, single client pair; no multi-symbol or fan-out contention.
- No thread pinning on macOS, so tail latency includes scheduler jitter
  (visible as the 34–56 µs maxima). On Linux with the `main.cpp` affinity
  settings, tails should tighten.
- Latency excludes the kernel network stack entirely.
- The order book's linear price-level scan (§5) means these numbers assume
  shallow-to-moderate book depth.
