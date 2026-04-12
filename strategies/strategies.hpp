#pragma once
#include "../include/strategy.hpp"
#include <deque>
#include <cmath>
#include <iostream>

namespace hft {

// ─── Mean Reversion (Bollinger Band) Strategy ─────────────────────────────────
//
// Logic:
//   - Maintain a rolling window of mid prices
//   - Compute moving average (MA) and standard deviation
//   - Place BUY limit at MA - k*sigma (lower band)  ← fills when price dips
//   - Place SELL limit at MA + k*sigma (upper band)  ← fills when price spikes
//   - Cancel and re-quote as bands shift
//   - Risk management: max position, stop loss
//
class MeanReversionStrategy : public Strategy {
public:
    struct Params {
        Params() = default;
        int    lookback      = 50;     // rolling window length
        double num_std       = 1.5;    // band width in sigmas
        double order_qty     = 0.01;   // BTC per order
        int    cooldown_ticks = 5;     // ticks between re-quotes
        double max_position  = 0.05;   // max net position
        double stop_loss_pct = 0.005;  // 0.5% stop loss
    };

    MeanReversionStrategy() {}
    explicit MeanReversionStrategy(Params p) : p_(p) {}

    std::string name() const override { return "MeanReversion"; }
    std::string description() const override {
        return "Bollinger Band mean reversion — buys lower band, sells upper band";
    }

    void on_start(ExchangeEmulator& ex, PnLTracker& tr) override {
        Strategy::on_start(ex, tr);
        std::cout << "[Strategy] " << name() << " started | "
                  << "lookback=" << p_.lookback
                  << " k=" << p_.num_std
                  << " qty=" << p_.order_qty << "\n";
    }

    void on_tick(const Tick& tick) override {
        symbol_ = tick.symbol;
        double mid = (tick.bid + tick.ask) / 2.0;
        window_.push_back(mid);
        if ((int)window_.size() > p_.lookback) window_.pop_front();

        ++tick_count_;
        last_tick_ = tick;

        // Stop loss check
        if (tracker_->position() > 0 && mid < entry_price_ * (1.0 - p_.stop_loss_pct)) {
            cancel_all();
            if (tracker_->position() > 0)
                market_sell(symbol_, tracker_->position());
            entry_price_ = 0;
            return;
        }
        if (tracker_->position() < 0 && mid > entry_price_ * (1.0 + p_.stop_loss_pct)) {
            cancel_all();
            if (tracker_->position() < 0)
                market_buy(symbol_, -tracker_->position());
            entry_price_ = 0;
            return;
        }

        if ((int)window_.size() < p_.lookback) return;  // warm-up period
        if (tick_count_ % p_.cooldown_ticks != 0) return;

        double ma  = compute_ma();
        double sig = compute_sigma(ma);
        if (sig < 1e-6) return;

        double lower = ma - p_.num_std * sig;
        double upper = ma + p_.num_std * sig;

        // Cancel stale quotes
        cancel_all();

        double pos = tracker_->position();

        // Only quote if within position limits
        if (pos < p_.max_position) {
            // BUY limit below current price — fills when market crosses down
            pending_buy_ = buy(symbol_, lower, p_.order_qty);
        }
        if (pos > -p_.max_position) {
            // SELL limit above current price — fills when market crosses up
            pending_sell_ = sell(symbol_, upper, p_.order_qty);
        }
    }

    void on_fill(const Trade& trade) override {
        entry_price_ = trade.price;
        if (trade.side == Side::BUY) {
            pending_buy_ = 0;
            // Place take-profit on the other side
        } else {
            pending_sell_ = 0;
        }
    }

    void on_end() override {
        cancel_all();
        double pos = tracker_->position();
        if (std::abs(pos) > 1e-6) {
            if (pos > 0) market_sell(symbol_, pos);
            else         market_buy(symbol_, -pos);
        }
    }

private:
    Params p_;
    std::deque<double> window_;
    std::string symbol_;
    Tick   last_tick_{};
    int    tick_count_   = 0;
    double entry_price_  = 0;
    OrderId pending_buy_ = 0, pending_sell_ = 0;

    double compute_ma() const {
        double sum = 0;
        for (double v : window_) sum += v;
        return sum / window_.size();
    }

    double compute_sigma(double ma) const {
        double var = 0;
        for (double v : window_) var += (v - ma) * (v - ma);
        return std::sqrt(var / window_.size());
    }

    void cancel_all() {
        if (pending_buy_)  { exchange_->cancel(pending_buy_);  pending_buy_  = 0; }
        if (pending_sell_) { exchange_->cancel(pending_sell_); pending_sell_ = 0; }
    }
};

// ─── Momentum Strategy ────────────────────────────────────────────────────────
//
// Logic:
//   - Detect trending price using short vs long EMA crossover
//   - Enter long when short EMA crosses above long EMA
//   - Enter short when short EMA crosses below long EMA
//   - Place limit orders slightly inside spread for better fills
//
class MomentumStrategy : public Strategy {
public:
    struct Params {
        Params() = default;
        int    short_period  = 10;
        int    long_period   = 30;
        double order_qty     = 0.01;
        double entry_offset_bps = 1.0;  // place limit this far from mid
        double max_position  = 0.05;
    };

    MomentumStrategy() {}
    explicit MomentumStrategy(Params p) : p_(p) {}

    std::string name()        const override { return "Momentum"; }
    std::string description() const override {
        return "EMA crossover momentum — long on bullish cross, short on bearish cross";
    }

    void on_tick(const Tick& tick) override {
        symbol_  = tick.symbol;
        double mid = (tick.bid + tick.ask) / 2.0;
        double alpha_s = 2.0 / (p_.short_period + 1);
        double alpha_l = 2.0 / (p_.long_period  + 1);

        if (ema_short_ == 0) { ema_short_ = mid; ema_long_ = mid; return; }
        ema_short_ = alpha_s * mid + (1 - alpha_s) * ema_short_;
        ema_long_  = alpha_l * mid + (1 - alpha_l) * ema_long_;

        bool bullish_cross = (ema_short_ > ema_long_) && (prev_short_ <= prev_long_);
        bool bearish_cross = (ema_short_ < ema_long_) && (prev_short_ >= prev_long_);
        prev_short_ = ema_short_;
        prev_long_  = ema_long_;

        double pos = tracker_->position();
        double offset = mid * p_.entry_offset_bps / 10000.0;

        if (bullish_cross && pos <= 0) {
            if (pending_) { exchange_->cancel(pending_); pending_ = 0; }
            if (std::abs(pos) > 1e-6) market_buy(symbol_, std::abs(pos));
            if (pos >= -p_.max_position)
                pending_ = buy(symbol_, mid - offset, p_.order_qty);
        }
        if (bearish_cross && pos >= 0) {
            if (pending_) { exchange_->cancel(pending_); pending_ = 0; }
            if (pos > 1e-6) market_sell(symbol_, pos);
            if (pos <= p_.max_position)
                pending_ = sell(symbol_, mid + offset, p_.order_qty);
        }
    }

    void on_fill(const Trade&) override { pending_ = 0; }

    void on_end() override {
        if (pending_) { exchange_->cancel(pending_); pending_ = 0; }
        double pos = tracker_->position();
        if (std::abs(pos) > 1e-6) {
            if (pos > 0) market_sell(symbol_, pos);
            else         market_buy(symbol_, -pos);
        }
    }

private:
    Params p_;
    std::string symbol_;
    double ema_short_ = 0, ema_long_ = 0;
    double prev_short_ = 0, prev_long_ = 0;
    OrderId pending_  = 0;
};

// ─── Market Making Strategy ───────────────────────────────────────────────────
//
// Logic:
//   - Continuously post limit buy and sell around fair value
//   - Earn the spread repeatedly
//   - Adjust quotes based on inventory (skew quotes to reduce position risk)
//
class MarketMakingStrategy : public Strategy {
public:
    struct Params {
        Params() = default;
        double half_spread_bps = 3.0;    // quote half-spread
        double order_qty       = 0.005;
        double skew_per_unit   = 0.5;    // skew quotes per unit of position
        double max_pos_usd     = 1000.0; // hard stop: max position in USD
        int    requote_ticks   = 10;
    };

    MarketMakingStrategy() {}
    explicit MarketMakingStrategy(Params p) : p_(p) {}
    std::string name() const override { return "MarketMaking"; }

    void on_tick(const Tick& tick) override {
        symbol_ = tick.symbol;
        ++tick_count_;

        double mid = (tick.bid + tick.ask) / 2.0;
        if (mid <= 0) return;

        // ── Cooldown after stop: wait before re-quoting ────────────────────
        if (stop_cooldown_ > 0) {
            --stop_cooldown_;
            return;
        }

        double pos = tracker_->position();
        double pos_value = std::abs(pos) * mid;

        // ── Hard stop-loss: flatten if position value exceeds limit ────────
        if (pos_value > p_.max_pos_usd) {
            if (pending_buy_)  { exchange_->cancel(pending_buy_);  pending_buy_  = 0; }
            if (pending_sell_) { exchange_->cancel(pending_sell_); pending_sell_ = 0; }
            if (pos > 0)  market_sell(symbol_, pos);
            else if (pos < 0) market_buy(symbol_, -pos);
            stop_cooldown_ = p_.requote_ticks * 10;  // pause 10x requote interval
            return;
        }

        if (tick_count_ % p_.requote_ticks != 0) return;

        double half = mid * p_.half_spread_bps / 10000.0;

        // Inventory skew: nudge quotes to reduce position risk
        double skew = pos * p_.skew_per_unit * mid / 10000.0;

        double bid_px = mid - half - skew;
        double ask_px = mid + half - skew;

        // Cancel and re-quote
        if (pending_buy_)  { exchange_->cancel(pending_buy_);  pending_buy_  = 0; }
        if (pending_sell_) { exchange_->cancel(pending_sell_); pending_sell_ = 0; }

        if (pos * mid <  p_.max_pos_usd)
            pending_buy_  = buy(symbol_, bid_px, p_.order_qty);
        if (pos * mid > -p_.max_pos_usd)
            pending_sell_ = sell(symbol_, ask_px, p_.order_qty);
    }

    void on_fill(const Trade& t) override {
        if (t.side == Side::BUY)  pending_buy_  = 0;
        else                       pending_sell_ = 0;
    }

    void on_end() override {
        if (pending_buy_)  exchange_->cancel(pending_buy_);
        if (pending_sell_) exchange_->cancel(pending_sell_);
        double pos = tracker_->position();
        if (std::abs(pos) > 1e-6) {
            if (pos > 0) market_sell(symbol_, pos);
            else         market_buy(symbol_, -pos);
        }
    }

private:
    Params p_;
    std::string symbol_;
    int     tick_count_    = 0;
    int     stop_cooldown_ = 0;
    OrderId pending_buy_   = 0, pending_sell_ = 0;
};

} // namespace hft
