#pragma once
#include "types.hpp"
#include <vector>
#include <map>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hft {

// ─── Per-trade record ─────────────────────────────────────────────────────────
struct TradeRecord {
    Timestamp open_ts, close_ts;
    Side      direction;
    Price     entry_price, exit_price;
    Volume    quantity;
    double    gross_pnl, net_pnl, commission;
    std::string strategy_id;
};

// ─── Equity point ─────────────────────────────────────────────────────────────
struct EquityPoint {
    Timestamp ts;
    double    equity;        // cash + mark-to-market position
    double    realised_pnl;
    double    unrealised_pnl;
    double    drawdown;
};

// ─── Summary stats ────────────────────────────────────────────────────────────
struct PerformanceStats {
    double initial_cash;
    double final_equity;
    double total_return_pct;
    double gross_pnl;
    double net_pnl;
    double total_commission;
    double max_drawdown_pct;
    double sharpe_ratio;          // annualised
    double sortino_ratio;
    double profit_factor;
    double win_rate_pct;
    int    total_trades;
    int    winning_trades;
    int    losing_trades;
    double avg_win;
    double avg_loss;
    double largest_win;
    double largest_loss;
    double avg_trade_duration_s;
    double max_position_held;
    Timestamp backtest_start, backtest_end;
    double     duration_hours;
};

// ─── PnL Tracker ─────────────────────────────────────────────────────────────
class PnLTracker {
public:
    explicit PnLTracker(double initial_cash, const std::string& symbol = "")
        : cash_(initial_cash), initial_cash_(initial_cash), symbol_(symbol) {}

    // ── Called by exchange on fill ─────────────────────────────────────────
    void on_fill(const Trade& trade) {
        double notional = trade.price * trade.quantity;
        double comm     = notional * commission_bps_ / 10000.0;

        if (trade.side == Side::BUY) {
            // Update average entry price (FIFO cost basis)
            if (position_ >= 0) {
                double total_cost = avg_entry_price_ * position_ + notional;
                position_ += trade.quantity;
                avg_entry_price_ = position_ > 0 ? total_cost / position_ : 0.0;
            } else {
                position_ += trade.quantity;
                if (position_ > 0)       avg_entry_price_ = trade.price;
                else if (position_ == 0) avg_entry_price_ = 0.0;
            }
            cash_ -= (notional + comm);
        } else {
            if (position_ <= 0) {
                double total_cost = avg_entry_price_ * (-position_) + notional;
                position_ -= trade.quantity;
                avg_entry_price_ = position_ < 0 ? total_cost / (-position_) : 0.0;
            } else {
                position_ -= trade.quantity;
                if (position_ < 0)       avg_entry_price_ = trade.price;
                else if (position_ == 0) avg_entry_price_ = 0.0;
            }
            cash_ += (notional - comm);
        }

        total_commission_ += comm;
        last_ts_ = trade.timestamp;
        max_position_held_ = std::max(max_position_held_, std::abs(position_));

        // Try to match open/close
        match_trades(trade);
    }

    // ── Called each tick to mark-to-market ────────────────────────────────
    void on_tick(const Tick& tick) {
        if (!symbol_.empty() && tick.symbol != symbol_) return;
        last_price_ = tick.last > 0 ? tick.last : (tick.bid + tick.ask) / 2.0;
        last_ts_    = tick.timestamp;

        if (equity_curve_.empty() || last_ts_ - equity_curve_.back().ts > 1'000'000'000LL) {
            record_equity();
        }
    }

    // ── Accessors ──────────────────────────────────────────────────────────
    double cash()           const { return cash_; }
    double position()       const { return position_; }
    double unrealised_pnl() const { return position_ * (last_price_ - avg_entry_price_); }
    double equity()         const { return cash_ + position_ * last_price_; }
    double realised_pnl()   const { return cash_ - initial_cash_; }
    double total_commission() const { return total_commission_; }

    const std::vector<EquityPoint>& equity_curve() const { return equity_curve_; }
    const std::vector<TradeRecord>& trade_records() const { return trade_records_; }

    void set_commission_bps(double bps) { commission_bps_ = bps; }

    // ── Compute performance statistics ─────────────────────────────────────
    PerformanceStats compute_stats() const {
        PerformanceStats s{};
        s.initial_cash     = initial_cash_;
        s.final_equity     = equity();
        s.total_return_pct = (s.final_equity - s.initial_cash) / s.initial_cash * 100.0;
        s.total_commission = total_commission_;
        s.total_trades     = (int)trade_records_.size();

        if (equity_curve_.size() >= 2) {
            s.backtest_start  = equity_curve_.front().ts;
            s.backtest_end    = equity_curve_.back().ts;
            s.duration_hours  = (s.backtest_end - s.backtest_start) / 3.6e12;
        }

        // PnL breakdown
        double gross_win = 0, gross_loss = 0;
        double sum_dur = 0;
        s.largest_win  = 0.0;
        s.largest_loss = 0.0;
        s.max_position_held = max_position_held_;

        for (const auto& tr : trade_records_) {
            s.net_pnl += tr.net_pnl;
            s.gross_pnl += tr.gross_pnl;
            sum_dur += (tr.close_ts - tr.open_ts) / 1e9;

            if (tr.net_pnl > 0) {
                ++s.winning_trades;
                gross_win += tr.net_pnl;
                s.largest_win = std::max(s.largest_win, tr.net_pnl);
            } else {
                ++s.losing_trades;
                gross_loss += std::abs(tr.net_pnl);
                s.largest_loss = std::min(s.largest_loss, tr.net_pnl);
            }
        }

        s.win_rate_pct   = s.total_trades > 0
                           ? 100.0 * s.winning_trades / s.total_trades : 0;
        s.avg_win        = s.winning_trades > 0 ? gross_win  / s.winning_trades : 0;
        s.avg_loss       = s.losing_trades  > 0 ? -gross_loss / s.losing_trades  : 0;
        s.profit_factor  = gross_loss > 0 ? gross_win / gross_loss : 0;
        s.avg_trade_duration_s = s.total_trades > 0 ? sum_dur / s.total_trades : 0;

        // Max drawdown
        s.max_drawdown_pct = compute_max_drawdown();

        // Sharpe / Sortino from equity returns
        auto [sharpe, sortino] = compute_risk_ratios();
        s.sharpe_ratio  = sharpe;
        s.sortino_ratio = sortino;

        return s;
    }

    // ── Print report ───────────────────────────────────────────────────────
    void print_report(std::ostream& out = std::cout) const {
        auto s = compute_stats();
        out << "\n";
        out << "╔══════════════════════════════════════════════════════════╗\n";
        out << "║          HFT BACKTEST PERFORMANCE REPORT                 ║\n";
        out << "╠══════════════════════════════════════════════════════════╣\n";
        auto line = [&](const std::string& label, const std::string& val) {
            out << "║  " << std::left << std::setw(30) << label
                << std::right << std::setw(24) << val << "  ║\n";
        };
        auto ld = [&](const std::string& l, double v, int p=2) {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(p) << v;
            line(l, ss.str());
        };

        out << "╠══ CAPITAL ════════════════════════════════════════════════╣\n";
        ld("Initial Cash ($)",         s.initial_cash, 2);
        ld("Final Equity ($)",          s.final_equity, 2);
        ld("Net PnL ($)",               s.net_pnl, 2);
        ld("Total Return (%)",          s.total_return_pct, 4);
        ld("Total Commission ($)",      s.total_commission, 4);

        out << "╠══ RISK METRICS ═══════════════════════════════════════════╣\n";
        ld("Max Drawdown (%)",          s.max_drawdown_pct, 2);
        ld("Sharpe Ratio (ann.)",       s.sharpe_ratio, 4);
        ld("Sortino Ratio (ann.)",      s.sortino_ratio, 4);
        ld("Profit Factor",             s.profit_factor, 4);

        out << "╠══ TRADE STATS ════════════════════════════════════════════╣\n";
        line("Total Trades",            std::to_string(s.total_trades));
        line("Winning Trades",          std::to_string(s.winning_trades));
        line("Losing Trades",           std::to_string(s.losing_trades));
        ld("Win Rate (%)",              s.win_rate_pct, 2);
        ld("Avg Win ($)",               s.avg_win, 4);
        ld("Avg Loss ($)",              s.avg_loss, 4);
        ld("Largest Win ($)",           s.largest_win, 2);
        ld("Largest Loss ($)",          s.largest_loss, 2);
        ld("Avg Trade Duration (s)",    s.avg_trade_duration_s, 1);

        out << "╠══ SIMULATION INFO ════════════════════════════════════════╣\n";
        ld("Backtest Duration (hrs)",   s.duration_hours, 2);
        out << "╚══════════════════════════════════════════════════════════╝\n\n";
    }

    // ── Export equity curve to CSV ─────────────────────────────────────────
    void export_equity_csv(const std::string& path) const {
        std::ofstream f(path);
        f << "timestamp_ns,equity,realised_pnl,unrealised_pnl,drawdown\n";
        f << std::fixed << std::setprecision(4);
        for (const auto& ep : equity_curve_) {
            f << ep.ts << ","
              << ep.equity << ","
              << ep.realised_pnl << ","
              << ep.unrealised_pnl << ","
              << ep.drawdown << "\n";
        }
    }

    // ── Export trade log to CSV ────────────────────────────────────────────
    void export_trades_csv(const std::string& path) const {
        std::ofstream f(path);
        f << "open_ts,close_ts,direction,entry_price,exit_price,"
             "quantity,gross_pnl,net_pnl,commission,strategy\n";
        f << std::fixed << std::setprecision(4);
        for (const auto& tr : trade_records_) {
            f << tr.open_ts << "," << tr.close_ts << ","
              << side_str(tr.direction) << ","
              << tr.entry_price << "," << tr.exit_price << ","
              << tr.quantity << ","
              << tr.gross_pnl << "," << tr.net_pnl << ","
              << tr.commission << "," << tr.strategy_id << "\n";
        }
    }

private:
    double cash_, initial_cash_;
    double position_ = 0.0;
    double avg_entry_price_ = 0.0;
    double last_price_ = 0.0;
    Timestamp last_ts_  = 0;
    double total_commission_ = 0.0;
    double commission_bps_   = 1.0;
    std::string symbol_;

    double peak_equity_        = 0;
    double max_position_held_  = 0;

    std::vector<EquityPoint> equity_curve_;
    std::vector<TradeRecord> trade_records_;

    // Simple FIFO matching for round-trip trades
    struct PendingLeg {
        Side      side;
        Price     price;
        Volume    qty;
        Timestamp ts;
        std::string strategy_id;
        double    comm;
    };
    std::vector<PendingLeg> open_legs_;

    void match_trades(const Trade& new_trade) {
        Volume remaining = new_trade.quantity;

        for (auto it = open_legs_.begin(); it != open_legs_.end() && remaining > 1e-9; ) {
            if (it->side == new_trade.side) { ++it; continue; }

            Volume matched = std::min(it->qty, remaining);
            double entry_comm = it->comm * (matched / it->qty);
            double exit_comm  = new_trade.price * matched * commission_bps_ / 10000.0;

            TradeRecord rec;
            rec.direction   = it->side;
            rec.entry_price = it->price;
            rec.exit_price  = new_trade.price;
            rec.open_ts     = it->ts;
            rec.close_ts    = new_trade.timestamp;
            rec.quantity    = matched;
            rec.gross_pnl   = it->side == Side::BUY
                              ? (rec.exit_price - rec.entry_price) * matched
                              : (rec.entry_price - rec.exit_price) * matched;
            rec.commission  = entry_comm + exit_comm;
            rec.net_pnl     = rec.gross_pnl - rec.commission;
            rec.strategy_id = new_trade.strategy_id;
            trade_records_.push_back(rec);

            it->qty  -= matched;
            it->comm -= entry_comm;
            remaining -= matched;

            if (it->qty < 1e-9)
                it = open_legs_.erase(it);
            else
                ++it;
        }

        if (remaining > 1e-9) {
            double comm = new_trade.price * remaining * commission_bps_ / 10000.0;
            open_legs_.push_back({ new_trade.side, new_trade.price, remaining,
                                   new_trade.timestamp, new_trade.strategy_id, comm });
        }
    }

    void record_equity() {
        double eq    = equity();
        double rpnl  = cash_ - initial_cash_;
        double upnl  = position_ * (last_price_ - avg_entry_price_);
        peak_equity_ = std::max(peak_equity_, eq);
        double dd    = peak_equity_ > 0 ? (peak_equity_ - eq) / peak_equity_ * 100.0 : 0;
        equity_curve_.push_back({ last_ts_, eq, rpnl, upnl, dd });
    }

    double compute_max_drawdown() const {
        double peak = -1e18, max_dd = 0;
        for (const auto& ep : equity_curve_) {
            peak = std::max(peak, ep.equity);
            if (peak < 1e-12) continue;
            double dd = (peak - ep.equity) / peak * 100.0;
            max_dd = std::max(max_dd, dd);
        }
        return max_dd;
    }

    std::pair<double,double> compute_risk_ratios() const {
        if (equity_curve_.size() < 2) return {0.0, 0.0};

        std::vector<double> returns;
        for (size_t i = 1; i < equity_curve_.size(); ++i) {
            double prev = equity_curve_[i-1].equity;
            if (prev > 0) {
                returns.push_back((equity_curve_[i].equity - prev) / prev);
            }
        }
        if (returns.empty()) return {0.0, 0.0};

        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double var  = 0;
        double dvar = 0; // downside variance
        for (double r : returns) {
            var  += (r - mean) * (r - mean);
            double d = std::min(r - mean, 0.0);
            dvar += d * d;
        }
        var  /= returns.size();
        dvar /= returns.size();

        double std_dev = std::sqrt(var);
        double dstd    = std::sqrt(dvar);

        // Annualise assuming ~1s intervals
        double ann = std::sqrt(365.0 * 24 * 3600);
        double sharpe  = std_dev  > 1e-10 ? (mean / std_dev)  * ann : 0.0;
        double sortino = dstd     > 1e-10 ? (mean / dstd)     * ann : 0.0;

        return {sharpe, sortino};
    }
};

} // namespace hft
