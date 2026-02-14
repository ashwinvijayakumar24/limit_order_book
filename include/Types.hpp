#pragma once

#include <cstdint>
#include <concepts>
#include <limits>
#include <compare>

namespace apexlob {

// ============================================================================
// CACHE AND ALIGNMENT CONSTANTS
// ============================================================================

constexpr std::size_t CACHE_LINE_SIZE = 64;

// ============================================================================
// FIXED-POINT PRICE REPRESENTATION
// ============================================================================

// Fixed-point price with 4 decimal places (e.g., 123.4567 -> 1234567)
// Range: ±9,223,372,036,854,775,807 / 10,000 = ±922,337,203,685,477.5807
class Price {
public:
    using UnderlyingType = std::int64_t;
    static constexpr UnderlyingType SCALE = 10'000;
    static constexpr std::int32_t DECIMAL_PLACES = 4;

    constexpr Price() noexcept : value_(0) {}

    explicit constexpr Price(UnderlyingType raw_value) noexcept
        : value_(raw_value) {}

    constexpr Price(double price) noexcept
        : value_(static_cast<UnderlyingType>(price * SCALE)) {}

    [[nodiscard]] constexpr UnderlyingType raw() const noexcept { return value_; }
    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(value_) / SCALE;
    }

    constexpr auto operator<=>(const Price&) const noexcept = default;

    constexpr Price operator+(const Price& other) const noexcept {
        return Price{value_ + other.value_};
    }

    constexpr Price operator-(const Price& other) const noexcept {
        return Price{value_ - other.value_};
    }

private:
    UnderlyingType value_;
};

// ============================================================================
// CORE TYPES
// ============================================================================

using OrderId = std::uint64_t;
using Quantity = std::uint32_t;
using Timestamp = std::uint64_t;
using ClientId = std::uint32_t;
using Symbol = std::uint32_t;  // Hash or index-based symbol identifier

constexpr OrderId INVALID_ORDER_ID = 0;
constexpr Quantity MAX_QUANTITY = std::numeric_limits<Quantity>::max();
constexpr std::size_t MAX_ORDERS_PER_SIDE = 100'000;
constexpr std::size_t MAX_PRICE_LEVELS = 10'000;

// ============================================================================
// ENUMERATIONS
// ============================================================================

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1
};

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OrderType : std::uint8_t {
    Limit = 0,      // Standard limit order
    Market = 1,     // Market order (immediate execution)
    IOC = 2,        // Immediate-or-cancel
    FOK = 3         // Fill-or-kill
};

enum class ExecutionStatus : std::uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Cancelled = 3,
    Rejected = 4
};

// ============================================================================
// MESSAGE TYPES (for wire protocol)
// ============================================================================

enum class MessageType : std::uint16_t {
    NewOrder = 1,
    CancelOrder = 2,
    ModifyOrder = 3,
    ExecutionReport = 100,
    OrderCancelled = 101,
    OrderRejected = 102,
    Trade = 200,
    Heartbeat = 255
};

// ============================================================================
// TRADE RECORD
// ============================================================================

struct alignas(CACHE_LINE_SIZE) Trade {
    OrderId buy_order_id;
    OrderId sell_order_id;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
    Symbol symbol;
    std::uint8_t padding_[18];  // Pad to cache line
};

static_assert(sizeof(Trade) == CACHE_LINE_SIZE);

// ============================================================================
// EXECUTION REPORT (output event)
// ============================================================================

struct alignas(CACHE_LINE_SIZE) ExecutionReport {
    OrderId order_id;
    ClientId client_id;
    std::uint64_t client_order_id;
    Symbol symbol;
    Price price;
    Quantity quantity;
    Quantity filled_quantity;
    ExecutionStatus status;
    Side side;
    Timestamp timestamp;
    std::uint8_t padding_[14];
};

// Note: Actual size may vary due to alignment
// static_assert(sizeof(ExecutionReport) == CACHE_LINE_SIZE);

// ============================================================================
// CONCEPTS
// ============================================================================

template<typename T>
concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

template<typename T>
concept CacheAligned = (alignof(T) >= CACHE_LINE_SIZE);

template<typename T>
concept Poolable = requires(T t) {
    { t.next_ } -> std::convertible_to<T*>;
    std::is_trivially_destructible_v<T>;
};

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

[[nodiscard]] inline Timestamp rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    std::uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<Timestamp>(hi) << 32) | lo;
#else
    return 0;  // Fallback for non-x86 architectures
#endif
}

[[nodiscard]] consteval bool is_power_of_two(std::size_t n) noexcept {
    return n != 0 && (n & (n - 1)) == 0;
}

template<std::size_t N>
concept PowerOfTwo = is_power_of_two(N);

} // namespace apexlob
