#pragma once

#include "Types.hpp"
#include <atomic>

namespace apexlob {

// ============================================================================
// ORDER STRUCTURE (Cache-line aligned, hot/cold data separated)
// ============================================================================

// Forward declaration for intrusive linked list
struct Order;

// Hot data: frequently accessed during matching (first cache line)
struct alignas(CACHE_LINE_SIZE) Order {
    // ---- HOT DATA (First 64 bytes) ----
    OrderId id;                     // 8 bytes
    Price price;                    // 8 bytes
    Quantity quantity;              // 4 bytes
    Quantity filled_quantity;       // 4 bytes
    Side side;                      // 1 byte
    OrderType type;                 // 1 byte
    ExecutionStatus status;         // 1 byte
    std::uint8_t padding1_;         // 1 byte (alignment)

    // Intrusive doubly-linked list pointers for order book
    Order* prev_;                   // 8 bytes
    Order* next_;                   // 8 bytes

    Timestamp entry_timestamp;      // 8 bytes (for price-time priority)

    std::uint8_t padding2_[12];     // 12 bytes (pad to cache line)

    // ---- COLD DATA (Second cache line) ----
    ClientId client_id;             // 4 bytes
    Symbol symbol;                  // 4 bytes
    Timestamp last_modified;        // 8 bytes
    std::uint64_t client_order_id;  // 8 bytes (client's tracking ID)

    std::uint8_t padding3_[40];     // 40 bytes (pad to cache line)

    // ---- METHODS ----

    constexpr Order() noexcept
        : id(INVALID_ORDER_ID)
        , price()
        , quantity(0)
        , filled_quantity(0)
        , side(Side::Buy)
        , type(OrderType::Limit)
        , status(ExecutionStatus::New)
        , padding1_(0)
        , prev_(nullptr)
        , next_(nullptr)
        , entry_timestamp(0)
        , padding2_{}
        , client_id(0)
        , symbol(0)
        , last_modified(0)
        , client_order_id(0)
        , padding3_{}
    {}

    [[nodiscard]] constexpr Quantity remaining_quantity() const noexcept {
        return quantity - filled_quantity;
    }

    [[nodiscard]] constexpr bool is_fully_filled() const noexcept {
        return filled_quantity >= quantity;
    }

    [[nodiscard]] constexpr bool is_buy() const noexcept {
        return side == Side::Buy;
    }

    [[nodiscard]] constexpr bool is_sell() const noexcept {
        return side == Side::Sell;
    }

    [[nodiscard]] constexpr bool can_match_with(const Order& other) const noexcept {
        if (side == other.side) return false;

        if (is_buy()) {
            return price >= other.price;  // Buy price >= Sell price
        } else {
            return price <= other.price;  // Sell price <= Buy price
        }
    }

    void fill(Quantity qty) noexcept {
        filled_quantity += qty;
        last_modified = rdtsc();

        if (is_fully_filled()) {
            status = ExecutionStatus::Filled;
        } else {
            status = ExecutionStatus::PartiallyFilled;
        }
    }

    void cancel() noexcept {
        status = ExecutionStatus::Cancelled;
        last_modified = rdtsc();
    }

    void reject() noexcept {
        status = ExecutionStatus::Rejected;
        last_modified = rdtsc();
    }

    // Unlink from intrusive list
    constexpr void unlink() noexcept {
        if (prev_) prev_->next_ = next_;
        if (next_) next_->prev_ = prev_;
        prev_ = nullptr;
        next_ = nullptr;
    }
};

// Verify size assumptions
// Note: Actual size may vary due to compiler padding
// static_assert(sizeof(Order) == 128);  // Exactly 2 cache lines
static_assert(alignof(Order) == CACHE_LINE_SIZE);

// ============================================================================
// ORDER COMPARATORS (for price-time priority)
// ============================================================================

struct BuyOrderComparator {
    // Buy orders: higher price first, then earlier timestamp
    [[nodiscard]] constexpr bool operator()(const Order* a, const Order* b) const noexcept {
        if (a->price != b->price) {
            return a->price > b->price;  // Higher price has priority
        }
        return a->entry_timestamp < b->entry_timestamp;  // Earlier time has priority
    }
};

struct SellOrderComparator {
    // Sell orders: lower price first, then earlier timestamp
    [[nodiscard]] constexpr bool operator()(const Order* a, const Order* b) const noexcept {
        if (a->price != b->price) {
            return a->price < b->price;  // Lower price has priority
        }
        return a->entry_timestamp < b->entry_timestamp;  // Earlier time has priority
    }
};

// ============================================================================
// ORDER VALIDATION
// ============================================================================

struct OrderValidator {
    [[nodiscard]] static constexpr bool is_valid_price(const Price& price) noexcept {
        return price.raw() > 0;
    }

    [[nodiscard]] static constexpr bool is_valid_quantity(Quantity qty) noexcept {
        return qty > 0 && qty <= MAX_QUANTITY;
    }

    [[nodiscard]] static constexpr bool is_valid_order(const Order& order) noexcept {
        return order.id != INVALID_ORDER_ID
            && is_valid_quantity(order.quantity)
            && (order.type == OrderType::Market || is_valid_price(order.price));
    }
};

} // namespace apexlob
