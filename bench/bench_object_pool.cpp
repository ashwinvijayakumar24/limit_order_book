// bench_object_pool.cpp
// Microbenchmarks for the lock-free ObjectPool: acquire/release cost vs. heap.

#include <benchmark/benchmark.h>
#include "../include/ObjectPool.hpp"
#include "../include/Order.hpp"

#include <memory>
#include <vector>

using namespace apexlob;

namespace {

constexpr std::size_t POOL_CAPACITY = 65'536;
using OrderPool = ObjectPool<Order, POOL_CAPACITY>;

// Acquire + immediately release: the steady-state hot-path pattern for
// marketable orders that never rest in the book.
void BM_PoolAcquireRelease(benchmark::State& state) {
    auto pool = std::make_unique<OrderPool>();
    for (auto _ : state) {
        Order* o = pool->acquire();
        benchmark::DoNotOptimize(o);
        pool->release(o);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PoolAcquireRelease);

// Baseline: same pattern using the general-purpose allocator.
// This is the number the "eliminated hot-path heap allocations" claim rests on.
void BM_HeapNewDelete(benchmark::State& state) {
    for (auto _ : state) {
        Order* o = new Order();
        benchmark::DoNotOptimize(o);
        delete o;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HeapNewDelete);

// Batch acquire then batch release: models a burst of resting orders.
// Batch size is swept so free-list traversal cost with depth is visible.
void BM_PoolBatchAcquireRelease(benchmark::State& state) {
    const std::size_t batch = static_cast<std::size_t>(state.range(0));
    auto pool = std::make_unique<OrderPool>();
    std::vector<Order*> held(batch, nullptr);

    for (auto _ : state) {
        for (std::size_t i = 0; i < batch; ++i) {
            held[i] = pool->acquire();
        }
        benchmark::DoNotOptimize(held.data());
        for (std::size_t i = 0; i < batch; ++i) {
            pool->release(held[i]);
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
}
BENCHMARK(BM_PoolBatchAcquireRelease)->Arg(64)->Arg(1024)->Arg(16384);

// Same batch pattern on the heap, for a like-for-like comparison.
void BM_HeapBatchNewDelete(benchmark::State& state) {
    const std::size_t batch = static_cast<std::size_t>(state.range(0));
    std::vector<Order*> held(batch, nullptr);

    for (auto _ : state) {
        for (std::size_t i = 0; i < batch; ++i) {
            held[i] = new Order();
        }
        benchmark::DoNotOptimize(held.data());
        for (std::size_t i = 0; i < batch; ++i) {
            delete held[i];
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
}
BENCHMARK(BM_HeapBatchNewDelete)->Arg(64)->Arg(1024)->Arg(16384);

}  // namespace
