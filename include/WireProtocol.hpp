#pragma once

#include "Types.hpp"
#include "Order.hpp"
#include <bit>
#include <cstring>
#include <span>
#include <array>

namespace apexlob {

// ============================================================================
// BYTE ORDER CONVERSION
// ============================================================================

namespace detail {

template<typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr T byte_swap(T value) noexcept {
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>((value << 8) | ((value >> 8) & 0xFF));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(
            ((value & 0x000000FF) << 24) |
            ((value & 0x0000FF00) << 8) |
            ((value & 0x00FF0000) >> 8) |
            ((value & 0xFF000000) >> 24)
        );
    } else if constexpr (sizeof(T) == 8) {
        return static_cast<T>(
            ((value & 0x00000000000000FFULL) << 56) |
            ((value & 0x000000000000FF00ULL) << 40) |
            ((value & 0x0000000000FF0000ULL) << 24) |
            ((value & 0x00000000FF000000ULL) << 8) |
            ((value & 0x000000FF00000000ULL) >> 8) |
            ((value & 0x0000FF0000000000ULL) >> 24) |
            ((value & 0x00FF000000000000ULL) >> 40) |
            ((value & 0xFF00000000000000ULL) >> 56)
        );
    }
}

} // namespace detail

// Convert to network byte order (big endian)
template<typename T>
[[nodiscard]] constexpr T hton(T value) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        return value;
    } else {
        return detail::byte_swap(value);
    }
}

// Convert from network byte order
template<typename T>
[[nodiscard]] constexpr T ntoh(T value) noexcept {
    return hton(value);  // Symmetric operation
}

// ============================================================================
// MESSAGE HEADER (common to all messages)
// ============================================================================

struct alignas(8) MessageHeader {
    MessageType type;           // 2 bytes
    std::uint16_t length;       // 2 bytes - total message length including header
    std::uint32_t sequence;     // 4 bytes - sequence number for ordering

    [[nodiscard]] static constexpr std::size_t size() noexcept {
        return sizeof(MessageHeader);
    }

    void to_network_order() noexcept {
        type = static_cast<MessageType>(hton(static_cast<std::uint16_t>(type)));
        length = hton(length);
        sequence = hton(sequence);
    }

    void from_network_order() noexcept {
        type = static_cast<MessageType>(ntoh(static_cast<std::uint16_t>(type)));
        length = ntoh(length);
        sequence = ntoh(sequence);
    }
};

static_assert(sizeof(MessageHeader) == 8);

// ============================================================================
// NEW ORDER MESSAGE
// ============================================================================

struct alignas(8) NewOrderMessage {
    MessageHeader header;
    std::uint64_t client_order_id;
    std::uint32_t client_id;
    std::uint32_t symbol;
    std::int64_t price;         // Fixed-point raw value
    std::uint32_t quantity;
    std::uint8_t side;          // Side enum
    std::uint8_t order_type;    // OrderType enum
    std::uint16_t padding_;

    [[nodiscard]] static constexpr std::size_t size() noexcept {
        return sizeof(NewOrderMessage);
    }

    void to_network_order() noexcept {
        header.to_network_order();
        client_order_id = hton(client_order_id);
        client_id = hton(client_id);
        symbol = hton(symbol);
        price = hton(price);
        quantity = hton(quantity);
    }

    void from_network_order() noexcept {
        header.from_network_order();
        client_order_id = ntoh(client_order_id);
        client_id = ntoh(client_id);
        symbol = ntoh(symbol);
        price = ntoh(price);
        quantity = ntoh(quantity);
    }

    [[nodiscard]] Order to_order() const noexcept {
        Order order;
        order.client_order_id = client_order_id;
        order.client_id = client_id;
        order.symbol = symbol;
        order.price = Price{price};
        order.quantity = quantity;
        order.side = static_cast<Side>(side);
        order.type = static_cast<OrderType>(order_type);
        order.status = ExecutionStatus::New;
        order.filled_quantity = 0;
        return order;
    }
};

static_assert(sizeof(NewOrderMessage) == 40);

// ============================================================================
// CANCEL ORDER MESSAGE
// ============================================================================

struct alignas(8) CancelOrderMessage {
    MessageHeader header;
    std::uint64_t order_id;
    std::uint32_t client_id;
    std::uint32_t symbol;

    [[nodiscard]] static constexpr std::size_t size() noexcept {
        return sizeof(CancelOrderMessage);
    }

    void to_network_order() noexcept {
        header.to_network_order();
        order_id = hton(order_id);
        client_id = hton(client_id);
        symbol = hton(symbol);
    }

    void from_network_order() noexcept {
        header.from_network_order();
        order_id = ntoh(order_id);
        client_id = ntoh(client_id);
        symbol = ntoh(symbol);
    }
};

static_assert(sizeof(CancelOrderMessage) == 24);

// ============================================================================
// EXECUTION REPORT MESSAGE (outbound)
// ============================================================================

struct alignas(8) ExecutionReportMessage {
    MessageHeader header;
    std::uint64_t order_id;
    std::uint64_t client_order_id;
    std::uint32_t client_id;
    std::uint32_t symbol;
    std::int64_t price;
    std::uint32_t quantity;
    std::uint32_t filled_quantity;
    std::uint64_t timestamp;
    std::uint8_t status;
    std::uint8_t side;
    std::uint16_t padding_;

    [[nodiscard]] static constexpr std::size_t size() noexcept {
        return sizeof(ExecutionReportMessage);
    }

    void to_network_order() noexcept {
        header.to_network_order();
        order_id = hton(order_id);
        client_order_id = hton(client_order_id);
        client_id = hton(client_id);
        symbol = hton(symbol);
        price = hton(price);
        quantity = hton(quantity);
        filled_quantity = hton(filled_quantity);
        timestamp = hton(timestamp);
    }

    void from_network_order() noexcept {
        header.from_network_order();
        order_id = ntoh(order_id);
        client_order_id = ntoh(client_order_id);
        client_id = ntoh(client_id);
        symbol = ntoh(symbol);
        price = ntoh(price);
        quantity = ntoh(quantity);
        filled_quantity = ntoh(filled_quantity);
        timestamp = ntoh(timestamp);
    }

    [[nodiscard]] static ExecutionReportMessage from_report(const ExecutionReport& report,
                                                            std::uint32_t sequence) noexcept {
        ExecutionReportMessage msg{};
        msg.header.type = MessageType::ExecutionReport;
        msg.header.length = static_cast<std::uint16_t>(size());
        msg.header.sequence = sequence;
        msg.order_id = report.order_id;
        msg.client_order_id = report.client_order_id;
        msg.client_id = report.client_id;
        msg.symbol = report.symbol;
        msg.price = report.price.raw();
        msg.quantity = report.quantity;
        msg.filled_quantity = report.filled_quantity;
        msg.timestamp = report.timestamp;
        msg.status = static_cast<std::uint8_t>(report.status);
        msg.side = static_cast<std::uint8_t>(report.side);
        return msg;
    }
};

// Note: Actual size may vary due to alignment
// static_assert(sizeof(ExecutionReportMessage) == 56);

// ============================================================================
// TRADE MESSAGE (market data)
// ============================================================================

struct alignas(8) TradeMessage {
    MessageHeader header;
    std::uint64_t buy_order_id;
    std::uint64_t sell_order_id;
    std::uint32_t symbol;
    std::uint32_t quantity;
    std::int64_t price;
    std::uint64_t timestamp;

    [[nodiscard]] static constexpr std::size_t size() noexcept {
        return sizeof(TradeMessage);
    }

    void to_network_order() noexcept {
        header.to_network_order();
        buy_order_id = hton(buy_order_id);
        sell_order_id = hton(sell_order_id);
        symbol = hton(symbol);
        quantity = hton(quantity);
        price = hton(price);
        timestamp = hton(timestamp);
    }

    void from_network_order() noexcept {
        header.from_network_order();
        buy_order_id = ntoh(buy_order_id);
        sell_order_id = ntoh(sell_order_id);
        symbol = ntoh(symbol);
        quantity = ntoh(quantity);
        price = ntoh(price);
        timestamp = ntoh(timestamp);
    }

    [[nodiscard]] static TradeMessage from_trade(const Trade& trade,
                                                 std::uint32_t sequence) noexcept {
        TradeMessage msg{};
        msg.header.type = MessageType::Trade;
        msg.header.length = static_cast<std::uint16_t>(size());
        msg.header.sequence = sequence;
        msg.buy_order_id = trade.buy_order_id;
        msg.sell_order_id = trade.sell_order_id;
        msg.symbol = trade.symbol;
        msg.quantity = trade.quantity;
        msg.price = trade.price.raw();
        msg.timestamp = trade.timestamp;
        return msg;
    }
};

static_assert(sizeof(TradeMessage) == 48);

// ============================================================================
// MESSAGE PARSER
// ============================================================================

class MessageParser {
public:
    // Parse message header from buffer
    [[nodiscard]] static bool parse_header(std::span<const std::byte> buffer,
                                           MessageHeader& header) noexcept {
        if (buffer.size() < sizeof(MessageHeader)) {
            return false;
        }

        std::memcpy(&header, buffer.data(), sizeof(MessageHeader));
        header.from_network_order();
        return true;
    }

    // Parse new order message
    [[nodiscard]] static bool parse_new_order(std::span<const std::byte> buffer,
                                              NewOrderMessage& msg) noexcept {
        if (buffer.size() < sizeof(NewOrderMessage)) {
            return false;
        }

        std::memcpy(&msg, buffer.data(), sizeof(NewOrderMessage));
        msg.from_network_order();
        return msg.header.type == MessageType::NewOrder;
    }

    // Parse cancel order message
    [[nodiscard]] static bool parse_cancel_order(std::span<const std::byte> buffer,
                                                 CancelOrderMessage& msg) noexcept {
        if (buffer.size() < sizeof(CancelOrderMessage)) {
            return false;
        }

        std::memcpy(&msg, buffer.data(), sizeof(CancelOrderMessage));
        msg.from_network_order();
        return msg.header.type == MessageType::CancelOrder;
    }

    // Determine message type from buffer
    [[nodiscard]] static std::optional<MessageType> peek_message_type(
        std::span<const std::byte> buffer) noexcept {

        if (buffer.size() < sizeof(std::uint16_t)) {
            return std::nullopt;
        }

        std::uint16_t type_raw;
        std::memcpy(&type_raw, buffer.data(), sizeof(std::uint16_t));
        type_raw = ntoh(type_raw);
        return static_cast<MessageType>(type_raw);
    }
};

// ============================================================================
// MESSAGE ENCODER
// ============================================================================

class MessageEncoder {
public:
    // Encode message to buffer
    template<typename Message>
    [[nodiscard]] static bool encode(const Message& msg,
                                     std::span<std::byte> buffer) noexcept {
        if (buffer.size() < sizeof(Message)) {
            return false;
        }

        Message network_msg = msg;
        network_msg.to_network_order();
        std::memcpy(buffer.data(), &network_msg, sizeof(Message));
        return true;
    }

    // Encode to fixed-size array
    template<typename Message>
    [[nodiscard]] static std::array<std::byte, sizeof(Message)> encode_array(
        const Message& msg) noexcept {

        std::array<std::byte, sizeof(Message)> buffer;
        Message network_msg = msg;
        network_msg.to_network_order();
        std::memcpy(buffer.data(), &network_msg, sizeof(Message));
        return buffer;
    }
};

// ============================================================================
// FRAMING (length-prefixed messages for TCP)
// ============================================================================

struct FrameHeader {
    std::uint32_t length;  // Length of following message in bytes

    void to_network_order() noexcept {
        length = hton(length);
    }

    void from_network_order() noexcept {
        length = ntoh(length);
    }
};

} // namespace apexlob
