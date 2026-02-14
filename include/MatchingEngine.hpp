#pragma once

#include "Types.hpp"
#include "Order.hpp"
#include "OrderBook.hpp"
#include "ObjectPool.hpp"
#include "SPSCRingBuffer.hpp"
#include <unordered_map>
#include <functional>
#include <span>

namespace apexlob {

// ============================================================================
// MATCHING ENGINE CONFIGURATION
// ============================================================================

struct MatchingEngineConfig {
    std::size_t max_orders = MAX_ORDERS_PER_SIDE * 2;
    std::size_t input_queue_size = 8192;      // Must be power of 2
    std::size_t output_queue_size = 16384;    // Must be power of 2
    std::size_t trade_queue_size = 8192;
    bool enable_self_match_prevention = true;
};

// ============================================================================
// MATCHING ENGINE (core component)
// ============================================================================

template<std::size_t MaxOrders = MAX_ORDERS_PER_SIDE * 2,
         std::size_t InputQueueSize = 8192,
         std::size_t OutputQueueSize = 16384,
         std::size_t TradeQueueSize = 8192>
    requires PowerOfTwo<InputQueueSize> &&
             PowerOfTwo<OutputQueueSize> &&
             PowerOfTwo<TradeQueueSize>
class MatchingEngine {
public:
    // Type aliases
    using OrderPool = ObjectPool<Order, MaxOrders>;
    using InputQueue = SPSCRingBuffer<Order*, InputQueueSize>;
    using OutputQueue = SPSCRingBuffer<ExecutionReport, OutputQueueSize>;
    using TradeQueue = SPSCRingBuffer<Trade, TradeQueueSize>;

    explicit MatchingEngine(Symbol symbol) noexcept
        : symbol_(symbol)
        , order_book_(symbol)
        , order_pool_()
        , next_order_id_(1)
        , order_map_()
    {
        // Pre-allocate order map capacity
        order_map_.reserve(MaxOrders);
    }

    // Non-copyable, non-movable
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // ---- MAIN PROCESSING LOOP ----

    // Process incoming orders from input queue
    std::size_t process_inbound_orders(InputQueue& input_queue,
                                       OutputQueue& output_queue,
                                       TradeQueue& trade_queue) noexcept {
        std::size_t processed = 0;
        Order* order = nullptr;

        // Batch process available orders
        while (input_queue.try_pop(order)) {
            process_order(order, output_queue, trade_queue);
            ++processed;
        }

        return processed;
    }

    // Process single order (for testing or direct submission)
    void process_order(Order* order,
                      OutputQueue& output_queue,
                      TradeQueue& trade_queue) noexcept {
        if (!OrderValidator::is_valid_order(*order)) {
            reject_order(order, output_queue);
            return;
        }

        // Assign order ID
        order->id = next_order_id_++;
        order->entry_timestamp = rdtsc();

        // Store in order map
        order_map_[order->id] = order;

        // Attempt matching
        match_order(order, output_queue, trade_queue);

        // Add to book if not fully filled
        if (!order->is_fully_filled() && order->status != ExecutionStatus::Cancelled) {
            if (order->type == OrderType::Limit) {
                order_book_.add_order(order);
                send_execution_report(order, output_queue);
            } else if (order->type == OrderType::IOC) {
                // Immediate-or-cancel: cancel remaining
                order->cancel();
                send_execution_report(order, output_queue);
            }
        }
    }

    // ---- ORDER MANAGEMENT ----

    bool cancel_order(OrderId order_id, OutputQueue& output_queue) noexcept {
        auto it = order_map_.find(order_id);
        if (it == order_map_.end()) {
            return false;
        }

        Order* order = it->second;
        order_book_.remove_order(order);
        order->cancel();

        send_execution_report(order, output_queue);
        order_map_.erase(it);
        order_pool_.release(order);

        return true;
    }

    // ---- ACCESSORS ----

    [[nodiscard]] const OrderBook<>& order_book() const noexcept {
        return order_book_;
    }

    [[nodiscard]] OrderPool& order_pool() noexcept {
        return order_pool_;
    }

    [[nodiscard]] Symbol symbol() const noexcept {
        return symbol_;
    }

    [[nodiscard]] std::size_t active_order_count() const noexcept {
        return order_map_.size();
    }

private:
    // ---- MATCHING LOGIC ----

    void match_order(Order* incoming_order,
                    OutputQueue& output_queue,
                    TradeQueue& trade_queue) noexcept {
        // Get opposite side for matching
        Order* resting_order = incoming_order->is_buy()
            ? order_book_.best_ask_order()
            : order_book_.best_bid_order();

        while (resting_order != nullptr && !incoming_order->is_fully_filled()) {
            // Check if prices cross
            if (!incoming_order->can_match_with(*resting_order)) {
                break;
            }

            // Check self-match prevention
            if (incoming_order->client_id == resting_order->client_id) {
                // Skip or cancel based on policy
                resting_order = resting_order->next_;
                continue;
            }

            // Execute trade
            execute_trade(incoming_order, resting_order, output_queue, trade_queue);

            // Get next resting order
            Order* next_resting = resting_order->next_;

            // Remove fully filled resting order
            if (resting_order->is_fully_filled()) {
                order_book_.remove_order(resting_order);
                send_execution_report(resting_order, output_queue);

                auto it = order_map_.find(resting_order->id);
                if (it != order_map_.end()) {
                    order_map_.erase(it);
                    order_pool_.release(resting_order);
                }
            } else {
                // Partially filled - update book quantities
                send_execution_report(resting_order, output_queue);
            }

            resting_order = next_resting;
        }
    }

    void execute_trade(Order* aggressive_order,
                      Order* passive_order,
                      OutputQueue& output_queue,
                      TradeQueue& trade_queue) noexcept {
        // Trade price is passive order price (price-time priority)
        const Price trade_price = passive_order->price;

        // Trade quantity is minimum of remaining quantities
        const Quantity old_aggressive_remaining = aggressive_order->remaining_quantity();
        const Quantity old_passive_remaining = passive_order->remaining_quantity();
        const Quantity trade_qty = std::min(old_aggressive_remaining, old_passive_remaining);

        // Update order fill quantities
        aggressive_order->fill(trade_qty);
        passive_order->fill(trade_qty);

        // Update book quantities
        order_book_.update_order_quantity(passive_order, old_passive_remaining);

        // Record trade
        order_book_.record_trade(trade_price, trade_qty);

        // Generate trade record
        Trade trade{
            .buy_order_id = aggressive_order->is_buy() ? aggressive_order->id : passive_order->id,
            .sell_order_id = aggressive_order->is_sell() ? aggressive_order->id : passive_order->id,
            .price = trade_price,
            .quantity = trade_qty,
            .timestamp = rdtsc(),
            .symbol = symbol_
        };

        trade_queue.try_push(trade);
    }

    void reject_order(Order* order, OutputQueue& output_queue) noexcept {
        order->reject();
        send_execution_report(order, output_queue);
        order_pool_.release(order);
    }

    void send_execution_report(const Order* order, OutputQueue& output_queue) noexcept {
        ExecutionReport report{
            .order_id = order->id,
            .client_id = order->client_id,
            .client_order_id = order->client_order_id,
            .symbol = order->symbol,
            .price = order->price,
            .quantity = order->quantity,
            .filled_quantity = order->filled_quantity,
            .status = order->status,
            .side = order->side,
            .timestamp = rdtsc()
        };

        output_queue.try_push(report);
    }

    // ---- DATA MEMBERS ----

    Symbol symbol_;
    OrderBook<> order_book_;
    OrderPool order_pool_;
    OrderId next_order_id_;
    std::unordered_map<OrderId, Order*> order_map_;
};

// ============================================================================
// MULTI-SYMBOL MATCHING ENGINE
// ============================================================================

template<std::size_t MaxSymbols = 100>
class MultiSymbolMatchingEngine {
public:
    using Engine = MatchingEngine<>;

    MultiSymbolMatchingEngine() noexcept {
        engines_.reserve(MaxSymbols);
    }

    // Get or create engine for symbol
    [[nodiscard]] Engine* get_engine(Symbol symbol) noexcept {
        auto it = engines_.find(symbol);
        if (it != engines_.end()) {
            return it->second.get();
        }

        // Create new engine
        auto engine = std::make_unique<Engine>(symbol);
        Engine* ptr = engine.get();
        engines_[symbol] = std::move(engine);
        return ptr;
    }

    [[nodiscard]] std::size_t engine_count() const noexcept {
        return engines_.size();
    }

private:
    std::unordered_map<Symbol, std::unique_ptr<Engine>> engines_;
};

// ============================================================================
// STATISTICS
// ============================================================================

struct MatchingEngineStats {
    std::size_t total_orders_processed;
    std::size_t total_trades;
    std::size_t total_cancellations;
    std::size_t total_rejections;
    std::size_t active_orders;
    Quantity total_volume;
    Timestamp avg_latency_ticks;
    Timestamp max_latency_ticks;
};

} // namespace apexlob
