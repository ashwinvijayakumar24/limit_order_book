// test_concurrency.cpp
// Multi-threaded tests that mirror src/main.cpp's topology. These exist mainly
// to give ThreadSanitizer something real to inspect: the lock-free ring buffers
// and the object pool are the only places where two threads touch the same
// memory, and neither ASan nor UBSan can see a data race.
//
// Build and run with:
//   cmake -B build-tsan -DENABLE_TSAN=ON && cmake --build build-tsan
//   ./build-tsan/apexlob_tests --gtest_filter='Concurrency*'

#include <gtest/gtest.h>
#include "../include/MatchingEngine.hpp"
#include "../include/SPSCRingBuffer.hpp"
#include "../include/ObjectPool.hpp"
#include "../include/Types.hpp"
#include "../include/Order.hpp"

#include <atomic>
#include <memory>
#include <thread>

using namespace apexlob;

namespace {
constexpr Symbol TEST_SYMBOL = 1;
}

// ============================================================================
// Ring buffer carrying the payload the real system uses
// ============================================================================
// The existing SPSCRingBufferTest covers int. This one uses Order*, which is
// what network -> matching actually carries, and reads through the pointer on
// the consumer side so the object's contents cross cores too.

TEST(ConcurrencyTest, RingBufferCarriesOrderPointersAcrossThreads) {
    constexpr std::size_t kItems = 50'000;

    auto ring = std::make_unique<SPSCRingBuffer<Order*, 1024>>();
    auto orders = std::make_unique<std::array<Order, 256>>();
    for (std::size_t i = 0; i < orders->size(); ++i) {
        (*orders)[i].quantity = static_cast<Quantity>(i);
    }

    std::atomic<bool> start{false};
    std::atomic<std::size_t> mismatches{0};

    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();

        for (std::size_t i = 0; i < kItems; ++i) {
            Order* got = nullptr;
            while (!ring->try_pop(got)) std::this_thread::yield();

            // Dereference on the consumer core: the object's cache lines were
            // last written by the producer core.
            if (got->quantity != static_cast<Quantity>(i % orders->size())) {
                mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < kItems; ++i) {
        Order* o = &(*orders)[i % orders->size()];
        while (!ring->try_push(o)) std::this_thread::yield();
    }

    consumer.join();
    EXPECT_EQ(mismatches.load(), 0u);
    EXPECT_TRUE(ring->is_empty());
}

// ============================================================================
// Object pool acquired on one thread, released on another
// ============================================================================
// This is the pool's real usage pattern in main.cpp: the network thread calls
// acquire(), the matching thread calls release(). Both mutate free_list_.

TEST(ConcurrencyTest, ObjectPoolAcquireAndReleaseOnDifferentThreads) {
    constexpr std::size_t kItems = 50'000;

    auto pool = std::make_unique<ObjectPool<Order, 1024>>();
    auto ring = std::make_unique<SPSCRingBuffer<Order*, 512>>();

    std::atomic<bool> start{false};
    std::atomic<std::size_t> acquired{0};
    std::atomic<std::size_t> released{0};

    // Consumer: drains the ring and returns each object to the pool.
    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();

        for (std::size_t i = 0; i < kItems; ++i) {
            Order* o = nullptr;
            while (!ring->try_pop(o)) std::this_thread::yield();
            pool->release(o);
            released.fetch_add(1, std::memory_order_relaxed);
        }
    });

    start.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < kItems; ++i) {
        Order* o = nullptr;
        while ((o = pool->acquire()) == nullptr) std::this_thread::yield();
        o->quantity = static_cast<Quantity>(i);
        acquired.fetch_add(1, std::memory_order_relaxed);
        while (!ring->try_push(o)) std::this_thread::yield();
    }

    consumer.join();

    EXPECT_EQ(acquired.load(), kItems);
    EXPECT_EQ(released.load(), kItems);
    // Every object handed out came back
    EXPECT_EQ(pool->allocated(), 0u);
}

// ============================================================================
// The full three-thread pipeline, as wired in src/main.cpp
// ============================================================================
// producer -> input_queue -> matching -> {output_queue, trade_queue} -> drain.
// Matched pairs, so every order either trades or rests briefly.

TEST(ConcurrencyTest, ThreeThreadPipelineProcessesAllOrders) {
    constexpr std::size_t kPairs = 10'000;

    using Engine = MatchingEngine<>;
    auto engine = std::make_unique<Engine>(TEST_SYMBOL);
    auto input   = std::make_unique<Engine::InputQueue>();
    auto output  = std::make_unique<Engine::OutputQueue>();
    auto trades  = std::make_unique<Engine::TradeQueue>();

    std::atomic<bool> producing{true};
    std::atomic<bool> matching{true};
    std::atomic<std::size_t> submitted{0};
    std::atomic<std::size_t> processed{0};
    std::atomic<std::size_t> reports{0};
    std::atomic<std::size_t> fills{0};

    // Matching thread: drains input, pushes results outward.
    std::thread matcher([&] {
        while (matching.load(std::memory_order_acquire) || !input->is_empty()) {
            processed.fetch_add(
                engine->process_inbound_orders(*input, *output, *trades),
                std::memory_order_relaxed);
        }
    });

    // Output thread: drains both outbound queues.
    std::thread drainer([&] {
        ExecutionReport report;
        Trade trade;
        while (matching.load(std::memory_order_acquire) ||
               !output->is_empty() || !trades->is_empty()) {
            while (output->try_pop(report)) {
                reports.fetch_add(1, std::memory_order_relaxed);
            }
            while (trades->try_pop(trade)) {
                fills.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Producer thread (this one): acquire from pool, push pointer.
    auto submit = [&](Side side, double px, Quantity qty, ClientId cid) {
        Order* o = nullptr;
        while ((o = engine->order_pool().acquire()) == nullptr) std::this_thread::yield();
        o->side = side;
        o->price = Price{px};
        o->quantity = qty;
        o->type = OrderType::Limit;
        o->client_id = cid;
        o->symbol = TEST_SYMBOL;
        o->filled_quantity = 0;
        o->status = ExecutionStatus::New;
        while (!input->try_push(o)) std::this_thread::yield();
        submitted.fetch_add(1, std::memory_order_relaxed);
    };

    for (std::size_t i = 0; i < kPairs; ++i) {
        submit(Side::Sell, 100.0, 10, /*client_id=*/2);
        submit(Side::Buy,  100.0, 10, /*client_id=*/1);
    }
    producing.store(false, std::memory_order_release);

    // Let the matcher catch up, then signal shutdown.
    while (processed.load(std::memory_order_relaxed) < submitted.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }
    matching.store(false, std::memory_order_release);

    matcher.join();
    drainer.join();

    EXPECT_EQ(submitted.load(), kPairs * 2);
    EXPECT_EQ(processed.load(), kPairs * 2);
    // Each pair crosses and produces exactly one trade
    EXPECT_EQ(fills.load(), kPairs);
    // Every order produces at least one execution report
    EXPECT_GE(reports.load(), kPairs * 2);
    // Book fully cleared, nothing left resting
    EXPECT_EQ(engine->active_order_count(), 0u);
}
