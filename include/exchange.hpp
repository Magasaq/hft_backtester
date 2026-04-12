#pragma once
#include "types.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <functional>
#include <iostream>
#include <iomanip>

namespace hft {

// ─── Fill Notification ────────────────────────────────────────────────────────
using FillHandler = std::function<void(const Trade&)>;
using RejectHandler = std::function<void(OrderId, const std::string&)>;

// ─── Exchange Emulator ────────────────────────────────────────────────────────
//
// Execution Criterion (as specified):
//   BUY  limit order fills when market price (ask) drops to or BELOW order price
//   SELL limit order fills when market price (bid) rises to or ABOVE order price
//
// Market orders fill immediately at current bid/ask with optional slippage.
//
class ExchangeEmulator {
public:
    struct Config {
        Config() = default;
        double commission_bps = 1.0;   // per side
        double slippage_bps   = 0.5;   // market impact
        bool   partial_fills  = false; // simplification: no partial fills
        bool   verbose        = false;
    };

    ExchangeEmulator() {}
    explicit ExchangeEmulator(Config cfg) : cfg_(cfg) {}

    void on_fill  (FillHandler   h) { fill_handlers_.push_back(std::move(h)); }
    void on_reject(RejectHandler h) { reject_handlers_.push_back(std::move(h)); }

    // ── Submit order ───────────────────────────────────────────────────────
    OrderId submit(Order order) {
        if (order.type == OrderType::LIMIT && order.price <= 0) {
            OrderId id = ++next_id_;
            for (auto& h : reject_handlers_) h(id, "Invalid price <= 0");
            return id;
        }
        order.id         = ++next_id_;
        order.created_at = current_ts_;
        order.updated_at = current_ts_;
        order.status     = OrderStatus::OPEN;

        if (cfg_.verbose)
            std::cout << "[Exchange] Order #" << order.id
                      << " " << side_str(order.side)
                      << " " << order.quantity << " @ "
                      << (order.type == OrderType::MARKET ? "MKT" : std::to_string(order.price))
                      << "\n";

        // Try immediate execution for market orders
        if (order.type == OrderType::MARKET) {
            execute_market_order(order);
        } else {
            // Check if limit can fill immediately (crossed book)
            if (can_fill_immediately(order)) {
                execute_limit_at_price(order, order.price);
            } else {
                open_orders_[order.id] = order;
            }
        }

        all_orders_[order.id] = order;
        return order.id;
    }

    // ── Cancel order ───────────────────────────────────────────────────────
    bool cancel(OrderId id) {
        auto it = open_orders_.find(id);
        if (it == open_orders_.end()) return false;
        it->second.status = OrderStatus::CANCELLED;
        it->second.updated_at = current_ts_;
        all_orders_[id].status = OrderStatus::CANCELLED;
        open_orders_.erase(it);
        if (cfg_.verbose)
            std::cout << "[Exchange] Cancelled order #" << id << "\n";
        return true;
    }

    // ── Process market tick (the core matching loop) ───────────────────────
    //
    // KEY CRITERION: fill when market price CROSSES the order level
    //   - BUY  : fill if ask <= order.price   (market came down to us)
    //   - SELL : fill if bid >= order.price   (market came up to us)
    //
    void process_tick(const Tick& tick) {
        current_ts_  = tick.timestamp;
        last_bid_    = tick.bid;
        last_ask_    = tick.ask;
        last_price_  = tick.last;

        std::vector<OrderId> to_fill;

        for (auto& [id, ord] : open_orders_) {
            if (ord.symbol != tick.symbol) continue;

            bool triggered = false;
            if (ord.side == Side::BUY  && tick.ask <= ord.price) triggered = true;
            if (ord.side == Side::SELL && tick.bid >= ord.price) triggered = true;

            if (triggered) to_fill.push_back(id);
        }

        for (auto id : to_fill) {
            auto& ord = open_orders_[id];
            // Fill at the order price (passive fill — market came to us)
            execute_limit_at_price(ord, ord.price);
            all_orders_[id] = ord;
            open_orders_.erase(id);
        }
    }

    // ── Accessors ──────────────────────────────────────────────────────────
    const Order* get_order(OrderId id) const {
        auto it = all_orders_.find(id);
        return it != all_orders_.end() ? &it->second : nullptr;
    }

    size_t open_order_count()  const { return open_orders_.size(); }
    size_t total_order_count() const { return all_orders_.size(); }

    Price last_bid()   const { return last_bid_; }
    Price last_ask()   const { return last_ask_; }
    Price last_price() const { return last_price_; }
    Price mid_price()  const { return (last_bid_ + last_ask_) / 2.0; }

    const std::unordered_map<OrderId, Order>& all_orders() const {
        return all_orders_;
    }

    size_t fills_count()  const { return fills_count_; }
    double total_commission() const { return total_commission_; }

private:
    Config cfg_;
    OrderId next_id_    = 0;
    Timestamp current_ts_ = 0;
    Price last_bid_    = 0, last_ask_ = 0, last_price_ = 0;
    size_t fills_count_  = 0;
    double total_commission_ = 0;

    std::unordered_map<OrderId, Order> open_orders_;
    std::unordered_map<OrderId, Order> all_orders_;
    std::vector<FillHandler>           fill_handlers_;
    std::vector<RejectHandler>         reject_handlers_;

    bool can_fill_immediately(const Order& o) const {
        if (o.side == Side::BUY  && last_ask_ > 0 && last_ask_ <= o.price) return true;
        if (o.side == Side::SELL && last_bid_ > 0 && last_bid_ >= o.price) return true;
        return false;
    }

    void execute_market_order(Order& ord) {
        Price fill_price = 0;
        double slip = cfg_.slippage_bps / 10000.0;
        if (ord.side == Side::BUY) {
            fill_price = last_ask_ > 0 ? last_ask_ * (1.0 + slip) : last_price_;
        } else {
            fill_price = last_bid_ > 0 ? last_bid_ * (1.0 - slip) : last_price_;
        }

        if (fill_price <= 0) {
            for (auto& h : reject_handlers_)
                h(ord.id, "No market price available");
            ord.status = OrderStatus::CANCELLED;
            return;
        }
        execute_at(ord, fill_price);
    }

    void execute_limit_at_price(Order& ord, Price fill_price) {
        // Apply small slippage on aggressive orders
        execute_at(ord, fill_price);
    }

    void execute_at(Order& ord, Price fill_price) {
        double comm = ord.quantity * fill_price * cfg_.commission_bps / 10000.0;
        total_commission_ += comm;

        ord.filled_qty  = ord.quantity;
        ord.status      = OrderStatus::FILLED;
        ord.updated_at  = current_ts_;
        ++fills_count_;

        Trade trade;
        trade.order_id    = ord.id;
        trade.symbol      = ord.symbol;
        trade.side        = ord.side;
        trade.price       = fill_price;
        trade.quantity    = ord.quantity;
        trade.timestamp   = current_ts_;
        trade.strategy_id = ord.strategy_id;

        if (cfg_.verbose)
            std::cout << "[Exchange] FILL #" << ord.id
                      << " " << side_str(ord.side)
                      << " " << ord.quantity << " @ " << fill_price
                      << " comm=" << std::fixed << std::setprecision(4) << comm
                      << "\n";

        for (auto& h : fill_handlers_) h(trade);
    }
};

} // namespace hft
