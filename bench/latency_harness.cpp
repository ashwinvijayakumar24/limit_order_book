// latency_harness.cpp
// End-to-end latency and throughput harness for ApexLOB.
//
// Google Benchmark reports mean/median wall time per operation. That is not
// what a trading system is judged on — tail latency is. This standalone
// harness records every individual sample and reports full percentiles.
//
// Two workloads are measured:
//
//   1. ENGINE-ONLY  — wire decode + MatchingEngine::process_order, on one
//      thread. Latency is measured from "bytes available" to "trade emitted".
//      This isolates the matching path from queueing and thread wakeup.
//
//   2. PIPELINE     — the deployed topology: a producer thread decodes wire
//      messages and pushes into the SPSC input ring, a matching thread drains
//      it and processes, a consumer thread drains the output ring. Latency is
//      measured from producer stamp to consumer receipt of the execution
//      report, so it includes both queue hops and cross-core visibility.
//
// Timing uses std::chrono::steady_clock, NOT the engine's rdtsc(): on arm64
// (Apple Silicon) Types.hpp's rdtsc() is a stub that returns 0, so the
// engine's own latency counters read zero on this platform.
//
// The t0 timestamp is carried through the pipeline in client_order_id, which
// the engine passes through untouched into the ExecutionReport.

#include "../include/MatchingEngine.hpp"
#include "../include/WireProtocol.hpp"
#include "../include/Order.hpp"
#include "../include/Types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace apexlob;
using Clock = std::chrono::steady_clock;

namespace {

constexpr Symbol BENCH_SYMBOL = 1;
constexpr std::int64_t BASE_PRICE_RAW = 1'000'000;  // 100.0000
constexpr Quantity ORDER_QTY = 100;

using Engine = MatchingEngine<>;

[[nodiscard]] std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

// ---- Percentile reporting ----

struct LatencyStats {
    std::size_t count = 0;
    double min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0, mean = 0;
};

LatencyStats summarize(std::vector<std::uint64_t>& samples_ns) {
    LatencyStats s;
    if (samples_ns.empty()) return s;
    std::sort(samples_ns.begin(), samples_ns.end());
    const auto at = [&](double q) {
        const std::size_t idx = static_cast<std::size_t>(
            q * static_cast<double>(samples_ns.size() - 1));
        return static_cast<double>(samples_ns[idx]);
    };
    long double sum = 0;
    for (std::uint64_t v : samples_ns) sum += static_cast<long double>(v);

    s.count = samples_ns.size();
    s.min = static_cast<double>(samples_ns.front());
    s.p50 = at(0.50);
    s.p90 = at(0.90);
    s.p99 = at(0.99);
    s.p999 = at(0.999);
    s.max = static_cast<double>(samples_ns.back());
    s.mean = static_cast<double>(sum / static_cast<long double>(samples_ns.size()));
    return s;
}

void print_stats(const std::string& label, const LatencyStats& s) {
    std::cout << "\n  " << label << "  (n = " << s.count << ")\n";
    std::cout << std::fixed << std::setprecision(3);
    const auto row = [](const char* name, double ns) {
        std::cout << "    " << std::left << std::setw(8) << name << std::right
                  << std::setw(12) << ns << " ns  "
                  << std::setw(10) << (ns / 1000.0) << " us\n";
    };
    row("min", s.min);
    row("mean", s.mean);
    row("p50", s.p50);
    row("p90", s.p90);
    row("p99", s.p99);
    row("p99.9", s.p999);
    row("max", s.max);
}

// ---- Wire message construction ----

// Build the on-the-wire byte image of a NewOrder, exactly as a client would
// send it. Decoding this is part of every measured sample.
std::array<std::byte, sizeof(NewOrderMessage)> build_wire_new_order(
    Side side, std::int64_t price_raw, Quantity qty, ClientId client_id,
    std::uint64_t client_order_id) {
    NewOrderMessage msg{};
    msg.header.type = MessageType::NewOrder;
    msg.header.length = static_cast<std::uint16_t>(NewOrderMessage::size());
    msg.header.sequence = 0;
    msg.client_order_id = client_order_id;
    msg.client_id = client_id;
    msg.symbol = BENCH_SYMBOL;
    msg.price = price_raw;
    msg.quantity = qty;
    msg.side = static_cast<std::uint8_t>(side);
    msg.order_type = static_cast<std::uint8_t>(OrderType::Limit);
    msg.to_network_order();

    std::array<std::byte, sizeof(NewOrderMessage)> wire{};
    std::memcpy(wire.data(), &msg, sizeof(NewOrderMessage));
    return wire;
}

// ============================================================================
// WORKLOAD 1: ENGINE-ONLY (decode + match, single thread)
// ============================================================================
//
// Each measured sample is one *aggressive* order that crosses and fully fills
// against one resting order. The resting order is seeded outside the timed
// region so the sample covers only the marketable path:
//     decode -> pool acquire -> validate -> match -> trade -> reports
//
void run_engine_only(std::size_t iterations, std::size_t warmup) {
    auto engine = std::make_unique<Engine>(BENCH_SYMBOL);
    auto output = std::make_unique<Engine::OutputQueue>();
    auto trades = std::make_unique<Engine::TradeQueue>();

    const auto resting_wire = build_wire_new_order(
        Side::Sell, BASE_PRICE_RAW, ORDER_QTY, /*client=*/2, /*coid=*/0);
    const auto aggressive_wire = build_wire_new_order(
        Side::Buy, BASE_PRICE_RAW, ORDER_QTY, /*client=*/1, /*coid=*/0);

    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);

    const auto drain = [&] {
        ExecutionReport r{};
        while (output->try_pop(r)) { }
        Trade t{};
        while (trades->try_pop(t)) { }
    };

    // Decode a wire buffer into a pooled Order, as the network thread does.
    const auto decode_into_pool = [&](const auto& wire) -> Order* {
        NewOrderMessage parsed{};
        if (!MessageParser::parse_new_order(std::span<const std::byte>(wire),
                                            parsed)) {
            return nullptr;
        }
        Order* o = engine->order_pool().acquire();
        if (o == nullptr) return nullptr;
        const Order decoded = parsed.to_order();
        o->side = decoded.side;
        o->price = decoded.price;
        o->quantity = decoded.quantity;
        o->filled_quantity = 0;
        o->type = decoded.type;
        o->status = ExecutionStatus::New;
        o->client_id = decoded.client_id;
        o->symbol = decoded.symbol;
        o->client_order_id = decoded.client_order_id;
        return o;
    };

    const std::size_t total = iterations + warmup;
    for (std::size_t i = 0; i < total; ++i) {
        // --- untimed setup: seed one resting sell order ---
        Order* resting = decode_into_pool(resting_wire);
        if (resting == nullptr) {
            std::cerr << "engine-only: pool exhausted at iteration " << i << "\n";
            break;
        }
        engine->process_order(resting, *output, *trades);
        drain();

        // --- timed: wire bytes in, trade out ---
        const auto t0 = Clock::now();
        Order* aggressive = decode_into_pool(aggressive_wire);
        if (aggressive == nullptr) {
            std::cerr << "engine-only: pool exhausted at iteration " << i << "\n";
            break;
        }
        engine->process_order(aggressive, *output, *trades);
        Trade trade{};
        const bool got_trade = trades->try_pop(trade);
        const auto t1 = Clock::now();

        if (!got_trade) {
            std::cerr << "engine-only: expected a trade at iteration " << i << "\n";
            break;
        }
        if (i >= warmup) {
            samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()));
        }
        drain();
    }

    auto stats = summarize(samples);
    std::cout << "\n=== WORKLOAD 1: ENGINE-ONLY (decode + match, 1 thread) ===";
    print_stats("order-to-trade latency", stats);
    if (stats.mean > 0) {
        std::cout << "    implied single-thread rate: "
                  << std::fixed << std::setprecision(0)
                  << (1e9 / stats.mean) << " matched orders/sec\n";
    }
}

// ============================================================================
// WORKLOAD 2: FULL PIPELINE (3 threads, 2 SPSC hops)
// ============================================================================
//
// producer thread : decode wire -> pool acquire -> push to input ring
// matching thread : drain input ring -> process_order -> push reports
// consumer thread : drain output ring -> compute latency from stamped t0
//
// Latency covers producer stamp -> consumer receipt of the execution report,
// i.e. decode + queue + match + queue + cross-core visibility.
//
void run_pipeline(std::size_t iterations, std::size_t warmup) {
    auto engine = std::make_unique<Engine>(BENCH_SYMBOL);
    auto input = std::make_unique<Engine::InputQueue>();
    auto output = std::make_unique<Engine::OutputQueue>();
    auto trades = std::make_unique<Engine::TradeQueue>();

    const std::size_t total = iterations + warmup;
    // Each pair is one resting sell + one aggressive buy.
    const std::size_t total_orders = total * 2;

    std::atomic<bool> start{false};
    std::atomic<bool> producer_done{false};
    std::atomic<std::size_t> matched_count{0};

    std::vector<std::uint64_t> samples;
    samples.reserve(total_orders);

    // ---- consumer: drains execution reports, measures latency ----
    // A matched pair emits three reports (resting fill, aggressive fill, and
    // the resting order's initial New), so the consumer cannot key off a fixed
    // report count. It runs until the matcher is finished and the ring is dry.
    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) { }
        ExecutionReport report{};
        for (;;) {
            if (output->try_pop(report)) {
                const std::uint64_t t0 = report.client_order_id;
                if (t0 != 0) {
                    const std::uint64_t t1 = now_ns();
                    samples.push_back(t1 > t0 ? t1 - t0 : 0);
                }
                continue;
            }
            if (producer_done.load(std::memory_order_acquire) &&
                matched_count.load(std::memory_order_acquire) >= total_orders) {
                // One final sweep to catch anything pushed after the check.
                while (output->try_pop(report)) {
                    const std::uint64_t t0 = report.client_order_id;
                    if (t0 != 0) {
                        const std::uint64_t t1 = now_ns();
                        samples.push_back(t1 > t0 ? t1 - t0 : 0);
                    }
                }
                break;
            }
        }
    });

    // ---- matching thread ----
    std::thread matcher([&] {
        while (!start.load(std::memory_order_acquire)) { }
        Trade trade{};
        while (matched_count.load(std::memory_order_relaxed) < total_orders) {
            Order* order = nullptr;
            if (input->try_pop(order)) {
                engine->process_order(order, *output, *trades);
                matched_count.fetch_add(1, std::memory_order_release);
            }
            while (trades->try_pop(trade)) { }  // keep the trade ring drained
        }
    });

    const auto resting_wire = build_wire_new_order(
        Side::Sell, BASE_PRICE_RAW, ORDER_QTY, /*client=*/2, /*coid=*/0);
    const auto aggressive_wire = build_wire_new_order(
        Side::Buy, BASE_PRICE_RAW, ORDER_QTY, /*client=*/1, /*coid=*/0);

    // ---- producer: decode + stamp + enqueue ----
    const auto decode_and_stamp = [&](const auto& wire, bool stamp) -> Order* {
        NewOrderMessage parsed{};
        if (!MessageParser::parse_new_order(std::span<const std::byte>(wire),
                                            parsed)) {
            return nullptr;
        }
        Order* o = engine->order_pool().acquire();
        if (o == nullptr) return nullptr;
        const Order decoded = parsed.to_order();
        o->side = decoded.side;
        o->price = decoded.price;
        o->quantity = decoded.quantity;
        o->filled_quantity = 0;
        o->type = decoded.type;
        o->status = ExecutionStatus::New;
        o->client_id = decoded.client_id;
        o->symbol = decoded.symbol;
        // Carry t0 in client_order_id; 0 means "do not measure this one".
        o->client_order_id = stamp ? now_ns() : 0;
        return o;
    };

    const auto wall_start = Clock::now();
    start.store(true, std::memory_order_release);

    bool pool_exhausted = false;
    for (std::size_t i = 0; i < total && !pool_exhausted; ++i) {
        const bool measure = (i >= warmup);
        // Resting order is never measured — it does not produce a fill.
        Order* resting = decode_and_stamp(resting_wire, /*stamp=*/false);
        Order* aggressive = decode_and_stamp(aggressive_wire, /*stamp=*/measure);
        if (resting == nullptr || aggressive == nullptr) {
            pool_exhausted = true;
            break;
        }
        while (!input->try_push(resting)) { }
        while (!input->try_push(aggressive)) { }
    }
    producer_done.store(true, std::memory_order_release);

    matcher.join();
    consumer.join();
    const auto wall_end = Clock::now();

    if (pool_exhausted) {
        std::cerr << "pipeline: order pool exhausted before completion\n";
    }

    const double seconds =
        std::chrono::duration<double>(wall_end - wall_start).count();
    const std::size_t processed = matched_count.load(std::memory_order_acquire);

    auto stats = summarize(samples);
    std::cout << "\n=== WORKLOAD 2: FULL PIPELINE (3 threads, 2 SPSC hops) ===";
    print_stats("order-to-execution-report latency", stats);
    std::cout << "\n    orders processed: " << processed << "\n";
    std::cout << "    wall time:        " << std::fixed << std::setprecision(4)
              << seconds << " s\n";
    std::cout << "    sustained rate:   " << std::fixed << std::setprecision(0)
              << (static_cast<double>(processed) / seconds)
              << " orders/sec\n";
}

// ============================================================================
// WORKLOAD 3: PIPELINE, CLOSED LOOP (unloaded latency)
// ============================================================================
//
// Workload 2 runs the producer flat out, so orders queue behind each other and
// the reported latency is dominated by queueing delay — it measures throughput
// capacity, not responsiveness. This workload keeps exactly ONE order in
// flight: the producer submits a pair, waits for the aggressive order's
// execution report to come back, and only then submits the next pair.
//
// This is the number that answers "how long does one order take end to end
// when the system is not backed up".
//
void run_pipeline_closed_loop(std::size_t iterations, std::size_t warmup) {
    auto engine = std::make_unique<Engine>(BENCH_SYMBOL);
    auto input = std::make_unique<Engine::InputQueue>();
    auto output = std::make_unique<Engine::OutputQueue>();
    auto trades = std::make_unique<Engine::TradeQueue>();

    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> last_latency_ns{0};
    std::atomic<std::size_t> completed{0};

    // ---- matching thread ----
    std::thread matcher([&] {
        Trade trade{};
        while (running.load(std::memory_order_acquire)) {
            Order* order = nullptr;
            if (input->try_pop(order)) {
                engine->process_order(order, *output, *trades);
            }
            while (trades->try_pop(trade)) { }
        }
    });

    // ---- consumer thread: stamps completion for the measured order ----
    std::thread consumer([&] {
        ExecutionReport report{};
        while (running.load(std::memory_order_acquire)) {
            if (output->try_pop(report)) {
                const std::uint64_t t0 = report.client_order_id;
                if (t0 != 0) {
                    const std::uint64_t t1 = now_ns();
                    last_latency_ns.store(t1 > t0 ? t1 - t0 : 0,
                                          std::memory_order_release);
                    completed.fetch_add(1, std::memory_order_release);
                }
            }
        }
    });

    const auto resting_wire = build_wire_new_order(
        Side::Sell, BASE_PRICE_RAW, ORDER_QTY, /*client=*/2, /*coid=*/0);
    const auto aggressive_wire = build_wire_new_order(
        Side::Buy, BASE_PRICE_RAW, ORDER_QTY, /*client=*/1, /*coid=*/0);

    const auto decode_and_stamp = [&](const auto& wire, bool stamp) -> Order* {
        NewOrderMessage parsed{};
        if (!MessageParser::parse_new_order(std::span<const std::byte>(wire),
                                            parsed)) {
            return nullptr;
        }
        Order* o = engine->order_pool().acquire();
        if (o == nullptr) return nullptr;
        const Order decoded = parsed.to_order();
        o->side = decoded.side;
        o->price = decoded.price;
        o->quantity = decoded.quantity;
        o->filled_quantity = 0;
        o->type = decoded.type;
        o->status = ExecutionStatus::New;
        o->client_id = decoded.client_id;
        o->symbol = decoded.symbol;
        o->client_order_id = stamp ? now_ns() : 0;
        return o;
    };

    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);

    const std::size_t total = iterations + warmup;
    for (std::size_t i = 0; i < total; ++i) {
        const std::size_t target = completed.load(std::memory_order_acquire) + 1;

        Order* resting = decode_and_stamp(resting_wire, /*stamp=*/false);
        Order* aggressive = decode_and_stamp(aggressive_wire, /*stamp=*/true);
        if (resting == nullptr || aggressive == nullptr) {
            std::cerr << "closed loop: pool exhausted at iteration " << i << "\n";
            break;
        }
        while (!input->try_push(resting)) { }
        while (!input->try_push(aggressive)) { }

        // Wait for this order's report before submitting the next one.
        while (completed.load(std::memory_order_acquire) < target) { }

        if (i >= warmup) {
            samples.push_back(last_latency_ns.load(std::memory_order_acquire));
        }
    }

    running.store(false, std::memory_order_release);
    matcher.join();
    consumer.join();

    auto stats = summarize(samples);
    std::cout << "\n=== WORKLOAD 3: PIPELINE, CLOSED LOOP (1 order in flight) ===";
    print_stats("unloaded order-to-execution-report latency", stats);
    std::cout << "\n    This is responsiveness at low load. Workload 2's higher\n"
                 "    numbers are queueing delay at saturation, not per-order cost.\n";
}

// Estimate the granularity of steady_clock, so sub-100ns percentiles can be
// read with the right amount of scepticism.
double measure_clock_resolution_ns() {
    std::uint64_t min_delta = ~std::uint64_t{0};
    for (int i = 0; i < 1000; ++i) {
        const auto a = Clock::now();
        Clock::time_point b;
        do {
            b = Clock::now();
        } while (b == a);
        const auto delta = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        if (delta > 0 && delta < min_delta) min_delta = delta;
    }
    return static_cast<double>(min_delta);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 200'000;
    std::size_t warmup = 10'000;
    if (argc > 1) iterations = static_cast<std::size_t>(std::stoull(argv[1]));
    if (argc > 2) warmup = static_cast<std::size_t>(std::stoull(argv[2]));

    std::cout << "ApexLOB latency harness\n";
    std::cout << "  iterations: " << iterations << "  warmup: " << warmup << "\n";
    std::cout << "  clock: std::chrono::steady_clock, measured resolution ~"
              << std::fixed << std::setprecision(1)
              << measure_clock_resolution_ns() << " ns\n";
#if defined(__x86_64__) || defined(_M_X64)
    std::cout << "  rdtsc: available (x86-64)\n";
#else
    std::cout << "  rdtsc: NOT available on this arch — Types.hpp rdtsc() "
                 "returns 0, so the engine's own latency counters are unusable "
                 "here; this harness uses steady_clock instead.\n";
#endif

    run_engine_only(iterations, warmup);
    run_pipeline(iterations, warmup);
    // Closed loop is slow (one round trip per sample); cap the sample count.
    run_pipeline_closed_loop(std::min<std::size_t>(iterations, 50'000),
                             std::min<std::size_t>(warmup, 2'000));

    std::cout << "\nNote: latencies are measured in-process. They exclude "
                 "kernel TCP receive, NIC, and network transit.\n";
    return 0;
}
