// test_matching_engine.cpp
// Integration tests for MatchingEngine — order types, fills, leaks, cancels

#include <gtest/gtest.h>
#include "../include/MatchingEngine.hpp"
#include "../include/Types.hpp"
#include "../include/Order.hpp"

using namespace apexlob;

// ============================================================================
// TEST FIXTURE
// ============================================================================

class MatchingEngineTest : public ::testing::Test {
protected:
    static constexpr Symbol TEST_SYMBOL = 1;

    void SetUp() override {
        engine       = std::make_unique<MatchingEngine<>>(TEST_SYMBOL);
        output_queue = std::make_unique<MatchingEngine<>::OutputQueue>();
        trade_queue  = std::make_unique<MatchingEngine<>::TradeQueue>();
    }

    // Acquire an order from the engine's pool with given fields
    Order* make_order(Side side, Price price, Quantity qty,
                      OrderType type = OrderType::Limit,
                      ClientId client_id = 1) {
        Order* o = engine->order_pool().acquire();
        EXPECT_NE(o, nullptr);
        if (!o) return nullptr;
        o->side          = side;
        o->price         = price;
        o->quantity      = qty;
        o->type          = type;
        o->client_id     = client_id;
        o->symbol        = TEST_SYMBOL;
        o->filled_quantity = 0;
        o->status        = ExecutionStatus::New;
        return o;
    }

    // Submit order directly to the matching engine (bypasses input queue)
    void submit(Order* o) {
        engine->process_order(o, *output_queue, *trade_queue);
    }

    // Drain and count trades
    std::size_t drain_trades() {
        std::size_t count = 0;
        Trade t;
        while (trade_queue->try_pop(t)) ++count;
        return count;
    }

    // Drain and count execution reports with given status
    std::size_t drain_reports(ExecutionStatus status) {
        std::size_t count = 0;
        ExecutionReport r;
        while (output_queue->try_pop(r)) {
            if (r.status == status) ++count;
        }
        return count;
    }

    // Drain all reports (returns total)
    std::size_t drain_all_reports() {
        std::size_t count = 0;
        ExecutionReport r;
        while (output_queue->try_pop(r)) ++count;
        return count;
    }

    std::unique_ptr<MatchingEngine<>>             engine;
    std::unique_ptr<MatchingEngine<>::OutputQueue> output_queue;
    std::unique_ptr<MatchingEngine<>::TradeQueue>  trade_queue;
};

// ============================================================================
// TESTS
// ============================================================================

// BUY 100 @ $100.07, SELL 100 @ $100.07 → 1 trade, both orders removed from book
TEST_F(MatchingEngineTest, BasicLimitMatch) {
    Order* buy  = make_order(Side::Buy,  Price{100.07}, 100, OrderType::Limit, /*client_id=*/1);
    Order* sell = make_order(Side::Sell, Price{100.07}, 100, OrderType::Limit, /*client_id=*/2);

    submit(buy);
    submit(sell);

    EXPECT_EQ(drain_trades(), 1u);
    EXPECT_EQ(engine->active_order_count(), 0u);
    EXPECT_EQ(engine->order_pool().allocated(), 0u);
}

// BUY 200 @ $100.07, SELL 100 @ $100.07 → 1 trade of 100, BUY remains with qty=100
TEST_F(MatchingEngineTest, PartialFill) {
    Order* sell = make_order(Side::Sell, Price{100.07}, 100, OrderType::Limit, /*client_id=*/2);
    Order* buy  = make_order(Side::Buy,  Price{100.07}, 200, OrderType::Limit, /*client_id=*/1);

    submit(sell);  // rests in book
    submit(buy);   // partially matches

    EXPECT_EQ(drain_trades(), 1u);
    // BUY remains in book with 100 unfilled
    EXPECT_EQ(engine->active_order_count(), 1u);

    // The resting BUY should have 100 filled, 100 remaining
    auto best_bid = engine->order_book().best_bid_order();
    ASSERT_NE(best_bid, nullptr);
    EXPECT_EQ(best_bid->remaining_quantity(), 100u);
    EXPECT_EQ(best_bid->filled_quantity, 100u);
}

// BUY 100 @ $100.05, SELL 100 @ $100.07 → spread, no trade, both rest in book
TEST_F(MatchingEngineTest, NoMatchBelowBid) {
    Order* buy  = make_order(Side::Buy,  Price{100.05}, 100);
    Order* sell = make_order(Side::Sell, Price{100.07}, 100);

    submit(buy);
    submit(sell);

    EXPECT_EQ(drain_trades(), 0u);
    EXPECT_EQ(engine->active_order_count(), 2u);
}

// SELL 50 @ $100.07 resting, IOC BUY 200 @ $100.07 → fills 50, cancels remaining 150
TEST_F(MatchingEngineTest, IOCOrderCancelsRemainder) {
    Order* sell = make_order(Side::Sell, Price{100.07}, 50,  OrderType::Limit, /*client_id=*/2);
    Order* buy  = make_order(Side::Buy,  Price{100.07}, 200, OrderType::IOC,   /*client_id=*/1);

    submit(sell);
    submit(buy);

    EXPECT_EQ(drain_trades(), 1u);
    // IOC is fully gone (filled 50 + cancelled 150 remainder)
    EXPECT_EQ(engine->active_order_count(), 0u);
    EXPECT_EQ(engine->order_pool().allocated(), 0u);
}

// SELL 200 @ $100.07 resting, FOK BUY 100 @ $100.07 → fills completely
TEST_F(MatchingEngineTest, FOKFullFill) {
    Order* sell = make_order(Side::Sell, Price{100.07}, 200, OrderType::Limit, /*client_id=*/2);
    Order* buy  = make_order(Side::Buy,  Price{100.07}, 100, OrderType::FOK,   /*client_id=*/1);

    submit(sell);
    submit(buy);

    EXPECT_EQ(drain_trades(), 1u);
    // Sell has 100 remaining, FOK buy is gone
    EXPECT_EQ(engine->active_order_count(), 1u);

    auto best_ask = engine->order_book().best_ask_order();
    ASSERT_NE(best_ask, nullptr);
    EXPECT_EQ(best_ask->remaining_quantity(), 100u);
}

// SELL 50 @ $100.07 resting, FOK BUY 200 @ $100.07 → no fill, order cancelled
TEST_F(MatchingEngineTest, FOKCancelIfInsufficient) {
    Order* sell = make_order(Side::Sell, Price{100.07}, 50,  OrderType::Limit, /*client_id=*/2);
    Order* buy  = make_order(Side::Buy,  Price{100.07}, 200, OrderType::FOK,   /*client_id=*/1);

    submit(sell);
    submit(buy);

    EXPECT_EQ(drain_trades(), 0u);
    // FOK cancelled, SELL remains in book
    EXPECT_EQ(engine->active_order_count(), 1u);

    // Verify the SELL order is still untouched in the book
    auto best_ask = engine->order_book().best_ask_order();
    ASSERT_NE(best_ask, nullptr);
    EXPECT_EQ(best_ask->quantity, 50u);
    EXPECT_EQ(best_ask->filled_quantity, 0u);
}

// SELL 100 @ $99.00 resting, Market BUY 100 → fills at $99.00 regardless of price
TEST_F(MatchingEngineTest, MarketOrderMatchesAtAnyPrice) {
    Order* sell    = make_order(Side::Sell, Price{99.00}, 100, OrderType::Limit,  /*client_id=*/2);
    Order* mkt_buy = make_order(Side::Buy,  Price{0.0},   100, OrderType::Market, /*client_id=*/1);

    submit(sell);
    submit(mkt_buy);

    EXPECT_EQ(drain_trades(), 1u);
    EXPECT_EQ(engine->active_order_count(), 0u);
    EXPECT_EQ(engine->order_pool().allocated(), 0u);
}

// client_id=1 posts SELL 100 @ $100, client_id=1 posts BUY 100 @ $100 → no trade
TEST_F(MatchingEngineTest, SelfMatchPrevention) {
    Order* sell = make_order(Side::Sell, Price{100.00}, 100, OrderType::Limit, /*client_id=*/1);
    Order* buy  = make_order(Side::Buy,  Price{100.00}, 100, OrderType::Limit, /*client_id=*/1);

    submit(sell);
    submit(buy);

    EXPECT_EQ(drain_trades(), 0u);
    // Both orders rest in book (self-match skipped)
    EXPECT_EQ(engine->active_order_count(), 2u);
}

// Submit 100 BUY-SELL pairs; verify pool is fully restored after all fills
TEST_F(MatchingEngineTest, PoolNotExhaustedAfterFills) {
    constexpr int PAIRS = 100;
    const std::size_t pool_capacity = engine->order_pool().capacity();

    // Submit BUY orders (client_id=1) — they rest in book
    for (int i = 0; i < PAIRS; ++i) {
        Order* buy = make_order(Side::Buy, Price{100.00}, 100, OrderType::Limit, /*client_id=*/1);
        submit(buy);
    }
    EXPECT_EQ(engine->active_order_count(), static_cast<std::size_t>(PAIRS));

    // Submit matching SELL orders (client_id=2) — each matches with a resting BUY
    for (int i = 0; i < PAIRS; ++i) {
        Order* sell = make_order(Side::Sell, Price{100.00}, 100, OrderType::Limit, /*client_id=*/2);
        submit(sell);
    }

    EXPECT_EQ(engine->active_order_count(), 0u);
    EXPECT_EQ(drain_trades(), static_cast<std::size_t>(PAIRS));
    EXPECT_EQ(engine->order_pool().allocated(), 0u);
    EXPECT_EQ(engine->order_pool().available(), pool_capacity);
}

// Add limit order to book, cancel it → removed from book, returned to pool
TEST_F(MatchingEngineTest, CancelOrder) {
    Order* buy = make_order(Side::Buy, Price{100.00}, 100);
    submit(buy);

    ASSERT_EQ(engine->active_order_count(), 1u);
    // The engine assigns order id starting at 1
    const OrderId assigned_id = 1;

    bool cancelled = engine->cancel_order(assigned_id, *output_queue);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(engine->active_order_count(), 0u);
    EXPECT_EQ(engine->order_pool().allocated(), 0u);

    // A Cancelled execution report should have been emitted
    EXPECT_GE(drain_reports(ExecutionStatus::Cancelled), 1u);
}

// ============================================================================
// REGRESSION: multi-level sweeping
// ============================================================================
// The intrusive next_ chain only links orders *within* one price level. Before
// the fix, match_order and can_fill_completely walked that chain and stopped at
// the end of the best level, so an aggressive order could never consume a
// second price level. That left unfilled quantity resting on the book at a
// price that crossed the other side.

TEST_F(MatchingEngineTest, SweepsAcrossTwoPriceLevels) {
    submit(make_order(Side::Sell, Price{50.00}, 50, OrderType::Limit, /*client_id=*/2));
    submit(make_order(Side::Sell, Price{51.00}, 50, OrderType::Limit, /*client_id=*/2));

    Order* buy = make_order(Side::Buy, Price{51.00}, 100, OrderType::Limit, /*client_id=*/1);
    submit(buy);

    // One trade per level consumed
    EXPECT_EQ(drain_trades(), 2u);
    // Everything filled, so nothing rests and the book is empty
    EXPECT_EQ(engine->active_order_count(), 0u);
    EXPECT_FALSE(engine->order_book().best_bid().has_value());
    EXPECT_FALSE(engine->order_book().best_ask().has_value());
}

TEST_F(MatchingEngineTest, SweepsAcrossThreePriceLevels) {
    submit(make_order(Side::Sell, Price{50.00}, 10, OrderType::Limit, /*client_id=*/2));
    submit(make_order(Side::Sell, Price{50.50}, 10, OrderType::Limit, /*client_id=*/2));
    submit(make_order(Side::Sell, Price{51.00}, 10, OrderType::Limit, /*client_id=*/2));

    submit(make_order(Side::Buy, Price{51.00}, 30, OrderType::Limit, /*client_id=*/1));

    EXPECT_EQ(drain_trades(), 3u);
    EXPECT_EQ(engine->active_order_count(), 0u);
}

TEST_F(MatchingEngineTest, SweepStopsAtLimitPrice) {
    submit(make_order(Side::Sell, Price{50.00}, 50, OrderType::Limit, /*client_id=*/2));
    submit(make_order(Side::Sell, Price{52.00}, 50, OrderType::Limit, /*client_id=*/2));

    // Only willing to pay 51 — must take the 50 level and stop, not the 52 level
    Order* buy = make_order(Side::Buy, Price{51.00}, 100, OrderType::Limit, /*client_id=*/1);
    submit(buy);

    EXPECT_EQ(drain_trades(), 1u);
    EXPECT_EQ(buy->filled_quantity, 50u);
    EXPECT_EQ(buy->remaining_quantity(), 50u);
    // Remainder rests as the best bid at 51, below the surviving 52 ask
    auto bid = engine->order_book().best_bid();
    auto ask = engine->order_book().best_ask();
    ASSERT_TRUE(bid.has_value());
    ASSERT_TRUE(ask.has_value());
    EXPECT_EQ(*bid, Price{51.00});
    EXPECT_EQ(*ask, Price{52.00});
}

TEST_F(MatchingEngineTest, SweepLeavesBookUncrossed) {
    submit(make_order(Side::Sell, Price{50.00}, 50, OrderType::Limit, /*client_id=*/2));
    submit(make_order(Side::Sell, Price{51.00}, 50, OrderType::Limit, /*client_id=*/2));

    submit(make_order(Side::Buy, Price{51.00}, 100, OrderType::Limit, /*client_id=*/1));

    // A bid and an ask at the same price must never coexist
    EXPECT_FALSE(engine->order_book().is_crossed());
}

TEST_F(MatchingEngineTest, SellSweepsAcrossTwoBidLevels) {
    submit(make_order(Side::Buy, Price{51.00}, 50, OrderType::Limit, /*client_id=*/2));
    submit(make_order(Side::Buy, Price{50.00}, 50, OrderType::Limit, /*client_id=*/2));

    submit(make_order(Side::Sell, Price{50.00}, 100, OrderType::Limit, /*client_id=*/1));

    EXPECT_EQ(drain_trades(), 2u);
    EXPECT_EQ(engine->active_order_count(), 0u);
    EXPECT_FALSE(engine->order_book().is_crossed());
}

TEST_F(MatchingEngineTest, MarketOrderSweepsAllLevels) {
    submit(make_order(Side::Sell, Price{50.00}, 25, OrderType::Limit, /*client_id=*/2));
    submit(make_order(Side::Sell, Price{60.00}, 25, OrderType::Limit, /*client_id=*/2));

    submit(make_order(Side::Buy, Price{0.0}, 50, OrderType::Market, /*client_id=*/1));

    EXPECT_EQ(drain_trades(), 2u);
    EXPECT_EQ(engine->active_order_count(), 0u);
}

TEST_F(MatchingEngineTest, FOKFillsAcrossTwoPriceLevels) {
    submit(make_order(Side::Sell, Price{50.00}, 50, OrderType::Limit, /*client_id=*/2));
    submit(make_order(Side::Sell, Price{51.00}, 50, OrderType::Limit, /*client_id=*/2));

    // Fillable only by sweeping both levels — the pre-scan must see both
    submit(make_order(Side::Buy, Price{51.00}, 100, OrderType::FOK, /*client_id=*/1));

    EXPECT_EQ(drain_trades(), 2u);
    EXPECT_EQ(engine->active_order_count(), 0u);
}

TEST_F(MatchingEngineTest, FOKStillKilledWhenBookTooThin) {
    submit(make_order(Side::Sell, Price{50.00}, 30, OrderType::Limit, /*client_id=*/2));
    submit(make_order(Side::Sell, Price{51.00}, 30, OrderType::Limit, /*client_id=*/2));

    // 60 available across both levels, 100 requested -> all-or-nothing kill
    submit(make_order(Side::Buy, Price{51.00}, 100, OrderType::FOK, /*client_id=*/1));

    EXPECT_EQ(drain_trades(), 0u);
    // Both sells still resting, untouched
    EXPECT_EQ(engine->active_order_count(), 2u);
}
