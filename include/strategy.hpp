#pragma once
#include "types.hpp"
#include "exchange.hpp"
#include "pnl_tracker.hpp"
#include <string>
#include <memory>

namespace hft {

// ─── Strategy Interface ───────────────────────────────────────────────────────
class Strategy {
public:
    virtual ~Strategy() = default;

    // Called once before replay starts
    virtual void on_start(ExchangeEmulator& exchange, PnLTracker& tracker) {
        exchange_ = &exchange;
        tracker_  = &tracker;
    }

    // Called on every market tick — primary decision point
    virtual void on_tick(const Tick& tick) = 0;

    // Called when one of the strategy's orders is filled
    virtual void on_fill(const Trade& trade) {}

    // Called at end of data — flatten positions
    virtual void on_end() {}

    // Metadata
    virtual std::string name() const = 0;
    virtual std::string description() const { return ""; }

protected:
    ExchangeEmulator* exchange_ = nullptr;
    PnLTracker*       tracker_  = nullptr;

    OrderId buy (const std::string& sym, Price price, Volume qty) {
        Order o;
        o.symbol      = sym;
        o.side        = Side::BUY;
        o.type        = OrderType::LIMIT;
        o.price       = price;
        o.quantity    = qty;
        o.strategy_id = name();
        return exchange_->submit(o);
    }

    OrderId sell(const std::string& sym, Price price, Volume qty) {
        Order o;
        o.symbol      = sym;
        o.side        = Side::SELL;
        o.type        = OrderType::LIMIT;
        o.price       = price;
        o.quantity    = qty;
        o.strategy_id = name();
        return exchange_->submit(o);
    }

    OrderId market_buy(const std::string& sym, Volume qty) {
        Order o;
        o.symbol      = sym;
        o.side        = Side::BUY;
        o.type        = OrderType::MARKET;
        o.quantity    = qty;
        o.strategy_id = name();
        return exchange_->submit(o);
    }

    OrderId market_sell(const std::string& sym, Volume qty) {
        Order o;
        o.symbol      = sym;
        o.side        = Side::SELL;
        o.type        = OrderType::MARKET;
        o.quantity    = qty;
        o.strategy_id = name();
        return exchange_->submit(o);
    }
};

} // namespace hft
