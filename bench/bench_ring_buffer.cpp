// bench_ring_buffer.cpp
// Microbenchmarks for SPSCRingBuffer: single-thread op cost and true
// cross-thread producer/consumer throughput (the "lock-free" claim).

#include <benchmark/benchmark.h>
#include "../include/SPSCRingBuffer.hpp"
#include "../include/Types.hpp"
#include "../include/Order.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

using namespace apexlob;

namespace {

constexpr std::size_t RING_CAPACITY = 8192;

// ---- Single-threaded op cost (no contention, cache-hot) ----

void BM_RingPushPop_SingleThread(benchmark::State& state) {
    auto ring = std::make_unique<SPSCRingBuffer<Order*, RING_CAPACITY>>();
    Order dummy{};
    Order* out = nullptr;

    for (auto _ : state) {
        benchmark::DoNotOptimize(ring->try_push(&dummy));
        benchmark::DoNotOptimize(ring->try_pop(out));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingPushPop_SingleThread);

// Same pattern through a std::mutex-guarded std::queue. This is the baseline
// behind "eliminating mutex contention" — uncontended, so it is the *cheapest*
// the mutex version can possibly be.
void BM_MutexQueuePushPop_SingleThread(benchmark::State& state) {
    std::queue<Order*> q;
    std::mutex m;
    Order dummy{};

    for (auto _ : state) {
        {
            std::lock_guard<std::mutex> lock(m);
            q.push(&dummy);
        }
        {
            std::lock_guard<std::mutex> lock(m);
            Order* out = q.front();
            q.pop();
            benchmark::DoNotOptimize(out);
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MutexQueuePushPop_SingleThread);

// ---- Cross-thread throughput: one producer thread, one consumer thread ----
// Measures sustained messages/sec through the queue, which is the structure
// used between the network, matching, and output threads.

template <typename Payload>
double spsc_throughput_msgs_per_sec(std::size_t total_messages) {
    auto ring = std::make_unique<SPSCRingBuffer<Payload, RING_CAPACITY>>();
    std::atomic<bool> start{false};

    const auto consumer = [&] {
        while (!start.load(std::memory_order_acquire)) { /* spin */ }
        std::size_t received = 0;
        Payload out{};
        while (received < total_messages) {
            if (ring->try_pop(out)) {
                benchmark::DoNotOptimize(out);
                ++received;
            }
        }
    };

    std::thread consumer_thread(consumer);
    const auto t0 = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    Payload item{};
    for (std::size_t sent = 0; sent < total_messages;) {
        if (ring->try_push(item)) ++sent;
    }
    consumer_thread.join();
    const auto t1 = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<double>(total_messages) / seconds;
}

// Order* payload: exactly what the network -> matching thread queue carries.
void BM_RingThroughput_CrossThread_OrderPtr(benchmark::State& state) {
    const auto total = static_cast<std::size_t>(state.range(0));
    double last_rate = 0.0;
    for (auto _ : state) {
        last_rate = spsc_throughput_msgs_per_sec<Order*>(total);
    }
    state.counters["msgs_per_sec"] =
        benchmark::Counter(last_rate, benchmark::Counter::kDefaults);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(total));
}
BENCHMARK(BM_RingThroughput_CrossThread_OrderPtr)
    ->Arg(1'000'000)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

// Trade payload (64 bytes, one cache line): matching -> output thread queue.
void BM_RingThroughput_CrossThread_Trade(benchmark::State& state) {
    const auto total = static_cast<std::size_t>(state.range(0));
    double last_rate = 0.0;
    for (auto _ : state) {
        last_rate = spsc_throughput_msgs_per_sec<Trade>(total);
    }
    state.counters["msgs_per_sec"] =
        benchmark::Counter(last_rate, benchmark::Counter::kDefaults);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(total));
}
BENCHMARK(BM_RingThroughput_CrossThread_Trade)
    ->Arg(1'000'000)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

// Mutex-guarded queue under *real* two-thread contention, same payload.
// This is the honest comparison for the mutex-elimination claim.
void BM_MutexQueueThroughput_CrossThread(benchmark::State& state) {
    const auto total = static_cast<std::size_t>(state.range(0));
    double last_rate = 0.0;

    for (auto _ : state) {
        std::queue<Order*> q;
        std::mutex m;
        std::atomic<bool> start{false};
        Order dummy{};

        std::thread consumer([&] {
            while (!start.load(std::memory_order_acquire)) { /* spin */ }
            std::size_t received = 0;
            while (received < total) {
                std::lock_guard<std::mutex> lock(m);
                if (!q.empty()) {
                    Order* out = q.front();
                    q.pop();
                    benchmark::DoNotOptimize(out);
                    ++received;
                }
            }
        });

        const auto t0 = std::chrono::steady_clock::now();
        start.store(true, std::memory_order_release);
        for (std::size_t sent = 0; sent < total;) {
            std::lock_guard<std::mutex> lock(m);
            if (q.size() < RING_CAPACITY) {
                q.push(&dummy);
                ++sent;
            }
        }
        consumer.join();
        const auto t1 = std::chrono::steady_clock::now();

        const double seconds = std::chrono::duration<double>(t1 - t0).count();
        last_rate = static_cast<double>(total) / seconds;
    }

    state.counters["msgs_per_sec"] =
        benchmark::Counter(last_rate, benchmark::Counter::kDefaults);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(total));
}
BENCHMARK(BM_MutexQueueThroughput_CrossThread)
    ->Arg(1'000'000)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

}  // namespace
