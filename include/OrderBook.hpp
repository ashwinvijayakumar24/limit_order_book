#pragma once

#include "Types.hpp"
#include "Order.hpp"
#include <array>
#include <optional>

namespace apexlob {

// ============================================================================
// PRICE LEVEL (intrusive doubly-linked list of orders at same price)
// ============================================================================

struct alignas(CACHE_LINE_SIZE) PriceLevel {
    Price price;                    // 8 bytes
    Quantity total_quantity;        // 4 bytes - aggregate quantity at this level
    std::uint32_t order_count;      // 4 bytes - number of orders
    Order* head;                    // 8 bytes - first order (best time priority)
    Order* tail;                    // 8 bytes - last order
    std::uint8_t padding_[32];      // 32 bytes - pad to cache line

    constexpr PriceLevel() noexcept
        : price()
        , total_quantity(0)
        , order_count(0)
        , head(nullptr)
        , tail(nullptr)
        , padding_{}
    {}

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return head == nullptr;
    }

    // Add order to end of queue (worst time priority)
    constexpr void push_back(Order* order) noexcept {
        order->prev_ = tail;
        order->next_ = nullptr;

        if (tail) {
            tail->next_ = order;
        } else {
            head = order;  // First order at this level
        }

        tail = order;
        total_quantity += order->remaining_quantity();
        ++order_count;
    }

    // Remove specific order from level
    constexpr void remove(Order* order) noexcept {
        if (order->prev_) {
            order->prev_->next_ = order->next_;
        } else {
            head = order->next_;  // Was first order
        }

        if (order->next_) {
            order->next_->prev_ = order->prev_;
        } else {
            tail = order->prev_;  // Was last order
        }

        total_quantity -= order->remaining_quantity();
        --order_count;

        order->prev_ = nullptr;
        order->next_ = nullptr;
    }

    // Update quantity when order is partially filled
    constexpr void update_quantity(Quantity old_qty, Quantity new_qty) noexcept {
        total_quantity = total_quantity - old_qty + new_qty;
    }
};

static_assert(sizeof(PriceLevel) == CACHE_LINE_SIZE);

// ============================================================================
// ORDER BOOK SIDE (buy or sell)
// ============================================================================

template<std::size_t MaxLevels = MAX_PRICE_LEVELS>
class OrderBookSide {
public:
    constexpr OrderBookSide() noexcept
        : levels_{}
        , level_count_(0)
        , best_level_idx_(MaxLevels)  // Invalid index
    {}

    // ---- ORDER OPERATIONS ----

    void add_order(Order* order) noexcept {
        const Price price = order->price;
        std::size_t level_idx = find_or_create_level(price);

        if (level_idx < MaxLevels) {
            levels_[level_idx].push_back(order);
            update_best_level();
        }
    }

    void remove_order(Order* order) noexcept {
        std::size_t level_idx = find_level(order->price);

        if (level_idx < MaxLevels) {
            levels_[level_idx].remove(order);

            // Remove empty level
            if (levels_[level_idx].is_empty()) {
                remove_level(level_idx);
            }

            update_best_level();
        }
    }

    void update_order_quantity(Order* order, Quantity old_remaining) noexcept {
        std::size_t level_idx = find_level(order->price);

        if (level_idx < MaxLevels) {
            levels_[level_idx].update_quantity(old_remaining, order->remaining_quantity());
        }
    }

    // ---- MARKET DATA QUERIES ----

    [[nodiscard]] const PriceLevel* best_level() const noexcept {
        if (best_level_idx_ < MaxLevels) {
            return &levels_[best_level_idx_];
        }
        return nullptr;
    }

    [[nodiscard]] Order* best_order() const noexcept {
        if (best_level_idx_ < MaxLevels) {
            return levels_[best_level_idx_].head;
        }
        return nullptr;
    }

    // First order at the best price level strictly worse than `price`.
    // Lets a sweeping order continue past a level it has exhausted; the
    // intrusive next_ chain only links orders within a single level.
    [[nodiscard]] Order* best_order_worse_than(const Price& price) const noexcept {
        std::size_t best = MaxLevels;

        for (std::size_t i = 0; i < level_count_; ++i) {
            if (levels_[i].is_empty()) continue;
            if (levels_[i].price == price) continue;
            if (is_better_price(levels_[i].price, price)) continue;

            if (best == MaxLevels ||
                is_better_price(levels_[i].price, levels_[best].price)) {
                best = i;
            }
        }

        return best < MaxLevels ? levels_[best].head : nullptr;
    }

    [[nodiscard]] std::optional<Price> best_price() const noexcept {
        const auto* level = best_level();
        if (level) {
            return level->price;
        }
        return std::nullopt;
    }

    [[nodiscard]] Quantity depth_at_level(std::size_t levels_deep) const noexcept {
        Quantity total = 0;
        std::size_t count = 0;

        for (std::size_t i = 0; i < level_count_ && count < levels_deep; ++i) {
            if (!levels_[i].is_empty()) {
                total += levels_[i].total_quantity;
                ++count;
            }
        }

        return total;
    }

    [[nodiscard]] std::size_t level_count() const noexcept {
        return level_count_;
    }

    [[nodiscard]] bool is_empty() const noexcept {
        return level_count_ == 0;
    }

private:
    // Find existing level with given price
    [[nodiscard]] std::size_t find_level(const Price& price) const noexcept {
        for (std::size_t i = 0; i < level_count_; ++i) {
            if (levels_[i].price == price) {
                return i;
            }
        }
        return MaxLevels;  // Not found
    }

    // Find or create level for price
    [[nodiscard]] std::size_t find_or_create_level(const Price& price) noexcept {
        // Check existing levels
        std::size_t idx = find_level(price);
        if (idx < MaxLevels) {
            return idx;
        }

        // Create new level
        if (level_count_ >= MaxLevels) {
            return MaxLevels;  // No space
        }

        idx = level_count_;
        levels_[idx].price = price;
        ++level_count_;

        return idx;
    }

    void remove_level(std::size_t idx) noexcept {
        if (idx >= level_count_) return;

        // Shift levels down
        for (std::size_t i = idx; i < level_count_ - 1; ++i) {
            levels_[i] = levels_[i + 1];
        }

        --level_count_;
        levels_[level_count_] = PriceLevel{};  // Clear last level
    }

    void update_best_level() noexcept {
        if (level_count_ == 0) {
            best_level_idx_ = MaxLevels;
            return;
        }

        // Linear search for best level
        // In production, use sorted data structure or heap
        best_level_idx_ = 0;
        for (std::size_t i = 1; i < level_count_; ++i) {
            if (is_better_price(levels_[i].price, levels_[best_level_idx_].price)) {
                best_level_idx_ = i;
            }
        }
    }

    // Override in derived classes for buy vs sell comparison
    [[nodiscard]] virtual bool is_better_price(const Price& a, const Price& b) const noexcept = 0;

protected:
    std::array<PriceLevel, MaxLevels> levels_;
    std::size_t level_count_;
    std::size_t best_level_idx_;
};

// ============================================================================
// BUY AND SELL SIDES (with price comparison logic)
// ============================================================================

template<std::size_t MaxLevels = MAX_PRICE_LEVELS>
class BuyBookSide : public OrderBookSide<MaxLevels> {
private:
    [[nodiscard]] bool is_better_price(const Price& a, const Price& b) const noexcept override {
        return a > b;  // Higher is better for buys
    }
};

template<std::size_t MaxLevels = MAX_PRICE_LEVELS>
class SellBookSide : public OrderBookSide<MaxLevels> {
private:
    [[nodiscard]] bool is_better_price(const Price& a, const Price& b) const noexcept override {
        return a < b;  // Lower is better for sells
    }
};

// ============================================================================
// COMPLETE ORDER BOOK
// ============================================================================

template<std::size_t MaxLevels = MAX_PRICE_LEVELS>
class OrderBook {
public:
    OrderBook(Symbol symbol) noexcept
        : symbol_(symbol)
        , bid_side_()
        , ask_side_()
        , last_trade_price_()
        , last_trade_quantity_(0)
        , total_volume_(0)
    {}

    // ---- ORDER LIFECYCLE ----

    void add_order(Order* order) noexcept {
        if (order->is_buy()) {
            bid_side_.add_order(order);
        } else {
            ask_side_.add_order(order);
        }
    }

    void remove_order(Order* order) noexcept {
        if (order->is_buy()) {
            bid_side_.remove_order(order);
        } else {
            ask_side_.remove_order(order);
        }
    }

    void update_order_quantity(Order* order, Quantity old_remaining) noexcept {
        if (order->is_buy()) {
            bid_side_.update_order_quantity(order, old_remaining);
        } else {
            ask_side_.update_order_quantity(order, old_remaining);
        }
    }

    // ---- MARKET DATA ----

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        return bid_side_.best_price();
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        return ask_side_.best_price();
    }

    [[nodiscard]] std::optional<Price> mid_price() const noexcept {
        auto bid = best_bid();
        auto ask = best_ask();

        if (bid && ask) {
            return Price{(bid->raw() + ask->raw()) / 2};
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Price> spread() const noexcept {
        auto bid = best_bid();
        auto ask = best_ask();

        if (bid && ask) {
            return *ask - *bid;
        }
        return std::nullopt;
    }

    [[nodiscard]] Order* best_bid_order() const noexcept {
        return bid_side_.best_order();
    }

    [[nodiscard]] Order* best_ask_order() const noexcept {
        return ask_side_.best_order();
    }

    // Next price level down/up, for orders sweeping through multiple levels
    [[nodiscard]] Order* best_bid_order_worse_than(const Price& price) const noexcept {
        return bid_side_.best_order_worse_than(price);
    }

    [[nodiscard]] Order* best_ask_order_worse_than(const Price& price) const noexcept {
        return ask_side_.best_order_worse_than(price);
    }

    [[nodiscard]] Symbol symbol() const noexcept {
        return symbol_;
    }

    [[nodiscard]] bool is_crossed() const noexcept {
        auto bid = best_bid();
        auto ask = best_ask();
        return bid && ask && *bid >= *ask;
    }

    void record_trade(const Price& price, Quantity quantity) noexcept {
        last_trade_price_ = price;
        last_trade_quantity_ = quantity;
        total_volume_ += quantity;
    }

    [[nodiscard]] Quantity total_volume() const noexcept {
        return total_volume_;
    }

private:
    Symbol symbol_;
    BuyBookSide<MaxLevels> bid_side_;
    SellBookSide<MaxLevels> ask_side_;
    Price last_trade_price_;
    Quantity last_trade_quantity_;
    Quantity total_volume_;
};

} // namespace apexlob
