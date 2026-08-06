// bench_wire_protocol.cpp
// Microbenchmarks for the fixed-width binary wire protocol, plus a
// tag=value (FIX-style) text parser baseline for a like-for-like comparison.
//
// The text baseline parses the same seven fields the binary NewOrderMessage
// carries (msg type, client order id, client id, symbol, price, quantity,
// side/type), from a realistic FIX-like SOH-delimited string.

#include <benchmark/benchmark.h>
#include "../include/WireProtocol.hpp"
#include "../include/Types.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

using namespace apexlob;

namespace {

NewOrderMessage make_new_order_message() {
    NewOrderMessage msg{};
    msg.header.type = MessageType::NewOrder;
    msg.header.length = static_cast<std::uint16_t>(NewOrderMessage::size());
    msg.header.sequence = 42;
    msg.client_order_id = 987654321ULL;
    msg.client_id = 7;
    msg.symbol = 1;
    msg.price = Price{123.4567}.raw();
    msg.quantity = 500;
    msg.side = static_cast<std::uint8_t>(Side::Buy);
    msg.order_type = static_cast<std::uint8_t>(OrderType::Limit);
    return msg;
}

// ---- Binary: encode ----

void BM_BinaryEncodeNewOrder(benchmark::State& state) {
    const NewOrderMessage msg = make_new_order_message();
    std::array<std::byte, sizeof(NewOrderMessage)> buffer{};

    for (auto _ : state) {
        benchmark::DoNotOptimize(
            MessageEncoder::encode(msg, std::span<std::byte>(buffer)));
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() *
                            static_cast<std::int64_t>(sizeof(NewOrderMessage)));
}
BENCHMARK(BM_BinaryEncodeNewOrder);

// ---- Binary: parse (the number the resume bullet is about) ----

void BM_BinaryParseNewOrder(benchmark::State& state) {
    NewOrderMessage msg = make_new_order_message();
    msg.to_network_order();

    std::array<std::byte, sizeof(NewOrderMessage)> wire{};
    std::memcpy(wire.data(), &msg, sizeof(NewOrderMessage));

    NewOrderMessage parsed{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(MessageParser::parse_new_order(
            std::span<const std::byte>(wire), parsed));
        benchmark::DoNotOptimize(parsed);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() *
                            static_cast<std::int64_t>(sizeof(NewOrderMessage)));
}
BENCHMARK(BM_BinaryParseNewOrder);

// Parse plus conversion into an Order struct — the full network-thread path.
void BM_BinaryParseNewOrderToOrder(benchmark::State& state) {
    NewOrderMessage msg = make_new_order_message();
    msg.to_network_order();

    std::array<std::byte, sizeof(NewOrderMessage)> wire{};
    std::memcpy(wire.data(), &msg, sizeof(NewOrderMessage));

    NewOrderMessage parsed{};
    for (auto _ : state) {
        if (MessageParser::parse_new_order(std::span<const std::byte>(wire), parsed)) {
            Order order = parsed.to_order();
            benchmark::DoNotOptimize(order);
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BinaryParseNewOrderToOrder);

// ---- Text (FIX-style tag=value) baseline ----

constexpr char SOH = '\x01';

// A representative FIX NewOrderSingle body carrying the same seven fields.
const std::string& fix_message() {
    static const std::string msg = [] {
        std::string s;
        s += "35=D"; s += SOH;          // MsgType = NewOrderSingle
        s += "11=987654321"; s += SOH;  // ClOrdID
        s += "49=7"; s += SOH;          // SenderCompID -> client id
        s += "55=1"; s += SOH;          // Symbol
        s += "44=123.4567"; s += SOH;   // Price
        s += "38=500"; s += SOH;        // OrderQty
        s += "54=1"; s += SOH;          // Side
        s += "40=2"; s += SOH;          // OrdType
        return s;
    }();
    return msg;
}

// Straightforward tag=value scanner: split on SOH, split on '=', convert with
// std::from_chars (faster than atoi/strtod-based parsing, so this baseline
// favours the text side rather than strawmanning it).
struct ParsedFixOrder {
    std::uint64_t client_order_id = 0;
    std::uint32_t client_id = 0;
    std::uint32_t symbol = 0;
    std::int64_t price_raw = 0;
    std::uint32_t quantity = 0;
    std::uint8_t side = 0;
    std::uint8_t order_type = 0;
    bool is_new_order = false;
};

bool parse_fix_new_order(std::string_view msg, ParsedFixOrder& out) noexcept {
    std::size_t pos = 0;
    while (pos < msg.size()) {
        const std::size_t end = msg.find(SOH, pos);
        if (end == std::string_view::npos) break;

        const std::string_view field = msg.substr(pos, end - pos);
        pos = end + 1;

        const std::size_t eq = field.find('=');
        if (eq == std::string_view::npos) continue;

        const std::string_view tag = field.substr(0, eq);
        const std::string_view value = field.substr(eq + 1);

        int tag_num = 0;
        if (std::from_chars(tag.data(), tag.data() + tag.size(), tag_num).ec != std::errc{}) {
            continue;
        }

        switch (tag_num) {
            case 35:  // MsgType
                out.is_new_order = (value == "D");
                break;
            case 11:
                std::from_chars(value.data(), value.data() + value.size(),
                                out.client_order_id);
                break;
            case 49:
                std::from_chars(value.data(), value.data() + value.size(),
                                out.client_id);
                break;
            case 55:
                std::from_chars(value.data(), value.data() + value.size(),
                                out.symbol);
                break;
            case 44: {
                // Decimal string -> int64 fixed-point with 4 decimal places.
                double as_double = 0.0;
                const std::size_t dot = value.find('.');
                std::int64_t whole = 0;
                std::from_chars(value.data(),
                                value.data() + (dot == std::string_view::npos
                                                    ? value.size()
                                                    : dot),
                                whole);
                std::int64_t frac = 0;
                int frac_digits = 0;
                if (dot != std::string_view::npos) {
                    const std::string_view frac_sv = value.substr(dot + 1);
                    frac_digits = static_cast<int>(frac_sv.size());
                    std::from_chars(frac_sv.data(), frac_sv.data() + frac_sv.size(), frac);
                }
                std::int64_t scale = 1;
                for (int i = 0; i < Price::DECIMAL_PLACES - frac_digits; ++i) scale *= 10;
                out.price_raw = whole * Price::SCALE + frac * scale;
                benchmark::DoNotOptimize(as_double);
                break;
            }
            case 38:
                std::from_chars(value.data(), value.data() + value.size(),
                                out.quantity);
                break;
            case 54: {
                unsigned v = 0;
                std::from_chars(value.data(), value.data() + value.size(), v);
                out.side = static_cast<std::uint8_t>(v == 1 ? 0 : 1);
                break;
            }
            case 40: {
                unsigned v = 0;
                std::from_chars(value.data(), value.data() + value.size(), v);
                out.order_type = static_cast<std::uint8_t>(v == 2 ? 0 : 1);
                break;
            }
            default:
                break;
        }
    }
    return out.is_new_order;
}

void BM_TextFixParseNewOrder(benchmark::State& state) {
    const std::string& msg = fix_message();
    ParsedFixOrder out{};

    for (auto _ : state) {
        out = ParsedFixOrder{};
        benchmark::DoNotOptimize(parse_fix_new_order(msg, out));
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() *
                            static_cast<std::int64_t>(msg.size()));
}
BENCHMARK(BM_TextFixParseNewOrder);

}  // namespace
