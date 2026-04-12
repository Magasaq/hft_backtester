#pragma once
#include "types.hpp"
#include "market_replay.hpp"
#include "exchange.hpp"
#include "pnl_tracker.hpp"
#include "strategy.hpp"
#include <vector>
#include <memory>
#include <chrono>
#include <iostream>

namespace hft {

// ─── Backtest Engine ──────────────────────────────────────────────────────────
//
// Orchestrates: DataReader → ReplayEngine → Exchange → Strategy → PnLTracker
//
class BacktestEngine {
public:
    explicit BacktestEngine(BacktestConfig cfg)
        : cfg_(cfg)
        , exchange_(ExchangeEmulator::Config{
            cfg.commission_bps, cfg.slippage_bps, false, cfg.verbose })
        , tracker_(cfg.initial_cash)
        , replay_(MarketReplayEngine::Config{
            cfg.start_ts, cfg.end_ts, 5, cfg.verbose }) {}

    // ── Attach a strategy ─────────────────────────────────────────────────
    void add_strategy(std::shared_ptr<Strategy> s) {
        strategies_.push_back(s);
    }

    // ── Load ticks directly (bypasses file I/O) ───────────────────────────
    void load_ticks(std::vector<Tick> ticks) {
        replay_.load_ticks(std::move(ticks));
    }

    // ── Run the full backtest ─────────────────────────────────────────────
    void run() {
        auto wall_start = std::chrono::high_resolution_clock::now();

        std::cout << "\n╔══════════════════════════════════════════════════╗\n";
        std::cout << "║         HFT BACKTEST ENGINE — STARTING           ║\n";
        std::cout << "╚══════════════════════════════════════════════════╝\n";
        std::cout << "  Data file   : " << cfg_.data_file << "\n";
        std::cout << "  Symbol      : " << cfg_.symbol    << "\n";
        std::cout << "  Initial $   : " << cfg_.initial_cash << "\n";
        std::cout << "  Strategies  : " << strategies_.size() << "\n\n";

        // ── 1. Load market data (skip if already loaded via load_ticks()) ──
        if (replay_.ticks_loaded() == 0 && !cfg_.data_file.empty()) {
            replay_.load_file(cfg_.data_file, cfg_.symbol);
        }
        if (replay_.ticks_loaded() == 0) {
            std::cerr << "[Engine] No data loaded. Aborting.\n";
            return;
        }

        // ── 2. Wire exchange fills → tracker & strategies ──────────────────
        exchange_.on_fill([this](const Trade& t) {
            tracker_.on_fill(t);
            for (auto& s : strategies_) s->on_fill(t);
        });

        tracker_.set_commission_bps(cfg_.commission_bps);

        // ── 3. Wire replay → exchange → strategies ─────────────────────────
        replay_.on_tick([this](const Tick& tick) {
            // Exchange processes first (fills pending orders)
            exchange_.process_tick(tick);
            // Tracker marks to market
            tracker_.on_tick(tick);
            // Strategies receive tick and may submit orders
            for (auto& s : strategies_) s->on_tick(tick);

            if (cfg_.verbose) {
                static size_t cnt = 0;
                if (++cnt % 10000 == 0)
                    std::cout << "[Engine] " << cnt << " ticks | equity=$"
                              << std::fixed << std::setprecision(2)
                              << tracker_.equity() << "\n";
            }
        });

        replay_.on_end([this]() {
            for (auto& s : strategies_) s->on_end();
        });

        // ── 4. Start strategies ────────────────────────────────────────────
        for (auto& s : strategies_) {
            s->on_start(exchange_, tracker_);
        }

        // ── 5. Replay ─────────────────────────────────────────────────────
        replay_.run();

        // ── 6. Print report ────────────────────────────────────────────────
        auto wall_end = std::chrono::high_resolution_clock::now();
        double wall_ms = std::chrono::duration<double, std::milli>(
            wall_end - wall_start).count();

        tracker_.print_report();

        double throughput = replay_.ticks_loaded() / (wall_ms / 1000.0);
        std::cout << std::fixed << std::setprecision(0);
        std::cout << "  Wall time : " << wall_ms << " ms\n";
        std::cout << "  Throughput: " << throughput << " ticks/sec\n\n";

        // ── 7. Export reports ──────────────────────────────────────────────
        if (!cfg_.data_file.empty()) {
            std::string base = "reports/";
            for (auto& s : strategies_) {
                tracker_.export_equity_csv(base + s->name() + "_equity.csv");
                tracker_.export_trades_csv(base + s->name() + "_trades.csv");
                std::cout << "  Reports → " << base << s->name() << "_*.csv\n";
            }
        }
    }

    // ── Accessors ──────────────────────────────────────────────────────────
    PnLTracker&       tracker()  { return tracker_; }
    ExchangeEmulator& exchange() { return exchange_; }

private:
    BacktestConfig    cfg_;
    ExchangeEmulator  exchange_;
    PnLTracker        tracker_;
    MarketReplayEngine replay_;
    std::vector<std::shared_ptr<Strategy>> strategies_;
};

} // namespace hft
