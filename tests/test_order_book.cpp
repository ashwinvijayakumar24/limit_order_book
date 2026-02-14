// test_order_book.cpp
// Unit tests for OrderBook component

#include <gtest/gtest.h>
#include "../include/OrderBook.hpp"
#include "../include/Order.hpp"
#include "../include/Types.hpp"

using namespace apexlob;

class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        symbol_ = 1;
        book_ = std::make_unique<OrderBook<>>(symbol_);
    }

    Symbol symbol_;
    std::unique_ptr<OrderBook<>> book_;
};

TEST_F(OrderBookTest, EmptyBook) {
    EXPECT_FALSE(book_->best_bid().has_value());
    EXPECT_FALSE(book_->best_ask().has_value());
}

TEST_F(OrderBookTest, AddBuyOrder) {
    Order order;
    order.id = 1;
    order.price = Price{100.0};
    order.quantity = 100;
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.status = ExecutionStatus::New;
    order.symbol = symbol_;

    book_->add_order(&order);

    auto best_bid = book_->best_bid();
    ASSERT_TRUE(best_bid.has_value());
    EXPECT_EQ(best_bid->raw(), Price{100.0}.raw());
}

TEST_F(OrderBookTest, AddSellOrder) {
    Order order;
    order.id = 1;
    order.price = Price{101.0};
    order.quantity = 100;
    order.side = Side::Sell;
    order.type = OrderType::Limit;
    order.status = ExecutionStatus::New;
    order.symbol = symbol_;

    book_->add_order(&order);

    auto best_ask = book_->best_ask();
    ASSERT_TRUE(best_ask.has_value());
    EXPECT_EQ(best_ask->raw(), Price{101.0}.raw());
}

TEST_F(OrderBookTest, PriceTimePriority_Buy) {
    // Add multiple buy orders - higher price should be best
    Order order1;
    order1.id = 1;
    order1.price = Price{100.0};
    order1.quantity = 100;
    order1.side = Side::Buy;
    order1.type = OrderType::Limit;
    order1.entry_timestamp = 1;
    order1.symbol = symbol_;

    Order order2;
    order2.id = 2;
    order2.price = Price{101.0};  // Higher price
    order2.quantity = 100;
    order2.side = Side::Buy;
    order2.type = OrderType::Limit;
    order2.entry_timestamp = 2;
    order2.symbol = symbol_;

    book_->add_order(&order1);
    book_->add_order(&order2);

    auto best_bid = book_->best_bid();
    ASSERT_TRUE(best_bid.has_value());
    EXPECT_EQ(best_bid->raw(), Price{101.0}.raw());  // Higher price wins
}

TEST_F(OrderBookTest, PriceTimePriority_Sell) {
    // Add multiple sell orders - lower price should be best
    Order order1;
    order1.id = 1;
    order1.price = Price{101.0};
    order1.quantity = 100;
    order1.side = Side::Sell;
    order1.type = OrderType::Limit;
    order1.entry_timestamp = 1;
    order1.symbol = symbol_;

    Order order2;
    order2.id = 2;
    order2.price = Price{100.0};  // Lower price
    order2.quantity = 100;
    order2.side = Side::Sell;
    order2.type = OrderType::Limit;
    order2.entry_timestamp = 2;
    order2.symbol = symbol_;

    book_->add_order(&order1);
    book_->add_order(&order2);

    auto best_ask = book_->best_ask();
    ASSERT_TRUE(best_ask.has_value());
    EXPECT_EQ(best_ask->raw(), Price{100.0}.raw());  // Lower price wins
}

TEST_F(OrderBookTest, RemoveOrder) {
    Order order;
    order.id = 1;
    order.price = Price{100.0};
    order.quantity = 100;
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.symbol = symbol_;

    book_->add_order(&order);
    EXPECT_TRUE(book_->best_bid().has_value());

    book_->remove_order(&order);
    EXPECT_FALSE(book_->best_bid().has_value());
}

TEST_F(OrderBookTest, MidPrice) {
    Order buy_order;
    buy_order.id = 1;
    buy_order.price = Price{100.0};
    buy_order.quantity = 100;
    buy_order.side = Side::Buy;
    buy_order.type = OrderType::Limit;
    buy_order.symbol = symbol_;

    Order sell_order;
    sell_order.id = 2;
    sell_order.price = Price{101.0};
    sell_order.quantity = 100;
    sell_order.side = Side::Sell;
    sell_order.type = OrderType::Limit;
    sell_order.symbol = symbol_;

    book_->add_order(&buy_order);
    book_->add_order(&sell_order);

    auto mid = book_->mid_price();
    ASSERT_TRUE(mid.has_value());
    EXPECT_EQ(mid->raw(), Price{100.5}.raw());
}

TEST_F(OrderBookTest, Spread) {
    Order buy_order;
    buy_order.id = 1;
    buy_order.price = Price{100.0};
    buy_order.quantity = 100;
    buy_order.side = Side::Buy;
    buy_order.type = OrderType::Limit;
    buy_order.symbol = symbol_;

    Order sell_order;
    sell_order.id = 2;
    sell_order.price = Price{101.0};
    sell_order.quantity = 100;
    sell_order.side = Side::Sell;
    sell_order.type = OrderType::Limit;
    sell_order.symbol = symbol_;

    book_->add_order(&buy_order);
    book_->add_order(&sell_order);

    auto spread = book_->spread();
    ASSERT_TRUE(spread.has_value());
    EXPECT_EQ(spread->raw(), Price{1.0}.raw());
}

TEST_F(OrderBookTest, RecordTrade) {
    book_->record_trade(Price{100.5}, 50);
    EXPECT_EQ(book_->total_volume(), 50);

    book_->record_trade(Price{100.6}, 25);
    EXPECT_EQ(book_->total_volume(), 75);
}
