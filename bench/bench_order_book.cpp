// bench_order_book.cpp
// Microbenchmarks for OrderBook insert / remove / best-price lookup.
//
// Note: BookSide::find_or_create_level does a LINEAR scan over price levels,
// so insert cost grows with the number of distinct price levels. The sweeps
// below make that cost explicit rather than hiding it behind a single number.

#include <benchmark/benchmark.h>
#include "../include/OrderBook.hpp"
#include "../include/ObjectPool.hpp"
#include "../include/Order.hpp"
#include "../include/Types.hpp"

#include <memory>
#include <vector>

using namespace apexlob;

namespace {

constexpr Symbol BENCH_SYMBOL = 1;
constexpr std::size_t POOL_CAPACITY = 65'536;
using OrderPool = ObjectPool<Order, POOL_CAPACITY>;

Order* make_order(OrderPool& pool, Side side, Price price, Quantity qty,
                  Timestamp ts) {
    Order* o = pool.acquire();
    if (o == nullptr) return nullptr;
    o->side = side;
    o->price = price;
    o->quantity = qty;
    o->filled_quantity = 0;
    o->type = OrderType::Limit;
    o->status = ExecutionStatus::New;
    o->client_id = 1;
    o->symbol = BENCH_SYMBOL;
    o->entry_timestamp = ts;
    return o;
}

// Insert into a book already holding `levels` distinct price levels.
// Each iteration adds one order and removes it again so the book stays at a
// fixed depth (otherwise the measurement would drift as the book grows).
void BM_OrderBookAddRemove(benchmark::State& state) {
    const std::size_t levels = static_cast<std::size_t>(state.range(0));

    auto pool = std::make_unique<OrderPool>();
    auto book = std::make_unique<OrderBook<>>(BENCH_SYMBOL);

    // Pre-populate one resting buy order per price level.
    std::vector<Order*> resting;
    resting.reserve(levels);
    for (std::size_t i = 0; i < levels; ++i) {
        Order* o = make_order(*pool, Side::Buy,
                              Price{static_cast<std::int64_t>(1'000'000 + i * 100)},
                              100, static_cast<Timestamp>(i));
        book->add_order(o);
        resting.push_back(o);
    }

    // The order that is repeatedly added/removed sits at a price level that
    // already exists, so this measures steady-state insert, not level creation.
    Order* churn = make_order(*pool, Side::Buy, Price{std::int64_t{1'000'000}},
                              100, static_cast<Timestamp>(levels));

    for (auto _ : state) {
        book->add_order(churn);
        book->remove_order(churn);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());

    for (Order* o : resting) {
        book->remove_order(o);
        pool->release(o);
    }
    pool->release(churn);
}
BENCHMARK(BM_OrderBookAddRemove)->Arg(1)->Arg(10)->Arg(100)->Arg(1000);

// Cost of creating a *new* price level (the linear-scan-then-append path).
void BM_OrderBookNewPriceLevel(benchmark::State& state) {
    const std::size_t levels = static_cast<std::size_t>(state.range(0));

    auto pool = std::make_unique<OrderPool>();
    auto book = std::make_unique<OrderBook<>>(BENCH_SYMBOL);

    std::vector<Order*> resting;
    resting.reserve(levels);
    for (std::size_t i = 0; i < levels; ++i) {
        Order* o = make_order(*pool, Side::Buy,
                              Price{static_cast<std::int64_t>(1'000'000 + i * 100)},
                              100, static_cast<Timestamp>(i));
        book->add_order(o);
        resting.push_back(o);
    }

    // Price is below every resting level, so find_or_create_level scans all
    // `levels` entries and then creates a new one.
    Order* churn = make_order(*pool, Side::Buy, Price{std::int64_t{1}}, 100, 0);

    for (auto _ : state) {
        book->add_order(churn);
        book->remove_order(churn);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());

    for (Order* o : resting) {
        book->remove_order(o);
        pool->release(o);
    }
    pool->release(churn);
}
BENCHMARK(BM_OrderBookNewPriceLevel)->Arg(1)->Arg(10)->Arg(100)->Arg(1000);

// Top-of-book lookup — hit on every incoming order during matching.
void BM_OrderBookBestBid(benchmark::State& state) {
    const std::size_t levels = 100;

    auto pool = std::make_unique<OrderPool>();
    auto book = std::make_unique<OrderBook<>>(BENCH_SYMBOL);

    std::vector<Order*> resting;
    resting.reserve(levels);
    for (std::size_t i = 0; i < levels; ++i) {
        Order* o = make_order(*pool, Side::Buy,
                              Price{static_cast<std::int64_t>(1'000'000 + i * 100)},
                              100, static_cast<Timestamp>(i));
        book->add_order(o);
        resting.push_back(o);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(book->best_bid());
    }
    state.SetItemsProcessed(state.iterations());

    for (Order* o : resting) {
        book->remove_order(o);
        pool->release(o);
    }
}
BENCHMARK(BM_OrderBookBestBid);

}  // namespace
