// bench_matching_engine.cpp
// Microbenchmarks for MatchingEngine::process_order across the cases that
// matter: an order that rests (no match), an order that fully matches one
// resting order, an order that sweeps N levels, and a cancel.
//
// These use MANUAL TIMING rather than PauseTiming/ResumeTiming. Each
// benchmark seeds the book outside the timed region, then times a whole
// batch of measured operations with steady_clock and reports the per-op
// average. PauseTiming costs hundreds of nanoseconds per call, which would
// dominate operations that themselves take tens of nanoseconds.

#include <benchmark/benchmark.h>
#include "../include/MatchingEngine.hpp"
#include "../include/Order.hpp"
#include "../include/Types.hpp"

#include <chrono>
#include <memory>
#include <vector>

using namespace apexlob;
using Clock = std::chrono::steady_clock;

namespace {

constexpr Symbol BENCH_SYMBOL = 1;
constexpr std::int64_t BASE_PRICE_RAW = 1'000'000;

using Engine = MatchingEngine<>;

struct EngineFixture {
    std::unique_ptr<Engine> engine;
    std::unique_ptr<Engine::OutputQueue> output;
    std::unique_ptr<Engine::TradeQueue> trades;

    EngineFixture()
        : engine(std::make_unique<Engine>(BENCH_SYMBOL))
        , output(std::make_unique<Engine::OutputQueue>())
        , trades(std::make_unique<Engine::TradeQueue>()) {}

    Order* make_order(Side side, Price price, Quantity qty, ClientId client_id,
                      OrderType type = OrderType::Limit) {
        Order* o = engine->order_pool().acquire();
        if (o == nullptr) return nullptr;
        o->side = side;
        o->price = price;
        o->quantity = qty;
        o->filled_quantity = 0;
        o->type = type;
        o->status = ExecutionStatus::New;
        o->client_id = client_id;
        o->symbol = BENCH_SYMBOL;
        return o;
    }

    void submit(Order* o) { engine->process_order(o, *output, *trades); }

    // Drain both output rings, as the output thread would, so try_push never
    // starts silently failing mid-measurement.
    void drain() {
        ExecutionReport report{};
        while (output->try_pop(report)) benchmark::DoNotOptimize(report);
        Trade trade{};
        while (trades->try_pop(trade)) benchmark::DoNotOptimize(trade);
    }
};

// Batch size per timed region. Large enough that clock resolution (~41 ns on
// Apple Silicon) is negligible against the total.
constexpr std::size_t BATCH = 2048;

// ---- Non-marketable limit order: validate, assign id, insert into book ----
// The book holds only buys, so nothing can cross. This is the passive path.
void BM_ProcessOrder_RestNoMatch(benchmark::State& state) {
    for (auto _ : state) {
        EngineFixture fx;  // fresh engine per iteration: the book only grows

        std::vector<Order*> batch;
        batch.reserve(BATCH);
        for (std::size_t i = 0; i < BATCH; ++i) {
            // All at one price level, so level lookup stays O(1)-ish and the
            // measurement isolates insert cost, not linear level search.
            Order* o = fx.make_order(Side::Buy, Price{BASE_PRICE_RAW}, 100, 1);
            if (o == nullptr) break;
            batch.push_back(o);
        }

        const auto t0 = Clock::now();
        for (Order* o : batch) fx.submit(o);
        const auto t1 = Clock::now();

        state.SetIterationTime(
            std::chrono::duration<double>(t1 - t0).count() /
            static_cast<double>(batch.size()));
        fx.drain();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessOrder_RestNoMatch)->UseManualTime();

// ---- One aggressive order fully matching exactly one resting order ----
// Resting orders are seeded untimed; only the aggressive side is measured.
void BM_ProcessOrder_FullMatchOneLevel(benchmark::State& state) {
    for (auto _ : state) {
        EngineFixture fx;
        const Price price{BASE_PRICE_RAW};

        // Seed BATCH resting sells from client 2 (untimed).
        for (std::size_t i = 0; i < BATCH; ++i) {
            Order* resting = fx.make_order(Side::Sell, price, 100, /*client=*/2);
            if (resting == nullptr) break;
            fx.submit(resting);
        }
        // Pre-acquire the aggressive orders so pool cost is not double counted
        // inside the timed region beyond the one acquire per order.
        std::vector<Order*> aggressive;
        aggressive.reserve(BATCH);
        for (std::size_t i = 0; i < BATCH; ++i) {
            Order* o = fx.make_order(Side::Buy, price, 100, /*client=*/1);
            if (o == nullptr) break;
            aggressive.push_back(o);
        }
        fx.drain();

        const auto t0 = Clock::now();
        for (Order* o : aggressive) fx.submit(o);  // each fully fills one resting
        const auto t1 = Clock::now();

        state.SetIterationTime(
            std::chrono::duration<double>(t1 - t0).count() /
            static_cast<double>(aggressive.size()));
        fx.drain();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessOrder_FullMatchOneLevel)->UseManualTime();

// ---- One aggressive order sweeping N resting orders across N levels ----
// Reported time is per SWEEP (not per fill); divide by the arg for per-fill.
void BM_ProcessOrder_SweepLevels(benchmark::State& state) {
    const auto depth = static_cast<std::size_t>(state.range(0));
    const std::size_t sweeps = 256;

    for (auto _ : state) {
        EngineFixture fx;

        // Seed `sweeps * depth` resting sells: depth distinct price levels,
        // `sweeps` orders deep at each level.
        bool ok = true;
        for (std::size_t s = 0; s < sweeps && ok; ++s) {
            for (std::size_t i = 0; i < depth; ++i) {
                Order* resting = fx.make_order(
                    Side::Sell,
                    Price{static_cast<std::int64_t>(BASE_PRICE_RAW + i * 100)},
                    100, /*client=*/2);
                if (resting == nullptr) { ok = false; break; }
                fx.submit(resting);
            }
        }

        std::vector<Order*> sweepers;
        sweepers.reserve(sweeps);
        for (std::size_t s = 0; s < sweeps; ++s) {
            // Priced through every level, sized to consume one order per level.
            Order* o = fx.make_order(
                Side::Buy,
                Price{static_cast<std::int64_t>(BASE_PRICE_RAW + depth * 100)},
                static_cast<Quantity>(100 * depth), /*client=*/1);
            if (o == nullptr) break;
            sweepers.push_back(o);
        }
        fx.drain();

        const auto t0 = Clock::now();
        for (Order* o : sweepers) fx.submit(o);
        const auto t1 = Clock::now();

        state.SetIterationTime(
            std::chrono::duration<double>(t1 - t0).count() /
            static_cast<double>(sweepers.size()));
        fx.drain();
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(depth));
}
BENCHMARK(BM_ProcessOrder_SweepLevels)
    ->Arg(1)->Arg(5)->Arg(20)->Arg(100)
    ->UseManualTime();

// ---- Cancel: hash lookup, unlink from level, report, pool release ----
void BM_CancelOrder(benchmark::State& state) {
    for (auto _ : state) {
        EngineFixture fx;
        const Price price{BASE_PRICE_RAW};

        std::vector<OrderId> ids;
        ids.reserve(BATCH);
        for (std::size_t i = 0; i < BATCH; ++i) {
            Order* o = fx.make_order(Side::Buy, price, 100, 1);
            if (o == nullptr) break;
            fx.submit(o);
            ids.push_back(o->id);
        }
        fx.drain();

        const auto t0 = Clock::now();
        for (OrderId id : ids) {
            benchmark::DoNotOptimize(fx.engine->cancel_order(id, *fx.output));
        }
        const auto t1 = Clock::now();

        state.SetIterationTime(
            std::chrono::duration<double>(t1 - t0).count() /
            static_cast<double>(ids.size()));
        fx.drain();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CancelOrder)->UseManualTime();

}  // namespace
