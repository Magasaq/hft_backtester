#include "include/types.hpp"
#include "include/data_reader.hpp"
#include "include/backtest_engine.hpp"
#include "strategies/strategies.hpp"

#include <iostream>
#include <string>
#include <sys/stat.h>

using namespace hft;

// ─── CLI config parsing ───────────────────────────────────────────────────────
BacktestConfig parse_config(int argc, char** argv) {
    BacktestConfig cfg;
    cfg.data_file      = "data/sample_ticks.csv";
    cfg.symbol         = "BTC-USD";
    cfg.commission_bps = 1.0;
    cfg.slippage_bps   = 0.5;
    cfg.initial_cash   = 100'000.0;
    cfg.verbose        = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data"       && i+1 < argc) cfg.data_file      = argv[++i];
        if (arg == "--symbol"     && i+1 < argc) cfg.symbol         = argv[++i];
        if (arg == "--cash"       && i+1 < argc) cfg.initial_cash   = std::stod(argv[++i]);
        if (arg == "--commission" && i+1 < argc) cfg.commission_bps = std::stod(argv[++i]);
        if (arg == "--verbose")                   cfg.verbose        = true;
    }
    return cfg;
}

// ─── Generate sample data if not present ─────────────────────────────────────
void ensure_sample_data(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return;

    std::cout << "[Main] Generating sample tick data → " << path << "\n";
    SampleDataGenerator::Params p;
    p.symbol      = "BTC-USD";
    p.n_ticks     = 50000;
    p.start_price = 65000.0;
    p.volatility  = 0.00025;
    p.drift       = 0.000005;
    p.spread_bps  = 2.0;
    p.tick_ns     = 100'000'000LL;   // 100ms tick interval
    p.seed        = 1337;
    SampleDataGenerator::generate_csv(path, p);
}

// ─── Run a single strategy backtest ──────────────────────────────────────────
template<typename StrategyT>
void run_backtest(const BacktestConfig& cfg, std::shared_ptr<StrategyT> strat) {
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  Strategy: " << strat->name() << "\n";
    std::cout << "  " << strat->description() << "\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    BacktestEngine engine(cfg);
    engine.add_strategy(strat);
    engine.run();
}

int main(int argc, char** argv) {
    std::cout << R"(
  ╦ ╦╔═╗╔╦╗  ╔╗ ╔═╗╔═╗╦╔═╔╦╗╔═╗╔═╗╔╦╗
  ╠═╣╠╣  ║   ╠╩╗╠═╣║  ╠╩╗ ║ ║╣ ╚═╗ ║
  ╩ ╩╚   ╩   ╚═╝╩ ╩╚═╝╩ ╩ ╩ ╚═╝╚═╝ ╩
     C++ HFT Backtesting Framework v1.0
)" << "\n";

    // Ensure output dirs
    mkdir("data",    0755);
    mkdir("reports", 0755);

    auto cfg = parse_config(argc, argv);
    ensure_sample_data(cfg.data_file);

    // ── Strategy 1: Mean Reversion (Bollinger Bands) ─────────────────────
    {
        MeanReversionStrategy::Params p;
        p.lookback       = 50;
        p.num_std        = 1.5;
        p.order_qty      = 0.01;
        p.cooldown_ticks = 3;
        p.max_position   = 0.05;
        p.stop_loss_pct  = 0.008;
        run_backtest(cfg, std::make_shared<MeanReversionStrategy>(p));
    }

    // ── Strategy 2: Momentum (EMA Crossover) ─────────────────────────────
    {
        MomentumStrategy::Params p;
        p.short_period     = 10;
        p.long_period      = 30;
        p.order_qty        = 0.01;
        p.entry_offset_bps = 1.0;
        p.max_position     = 0.04;
        run_backtest(cfg, std::make_shared<MomentumStrategy>(p));
    }

    // ── Strategy 3: Market Making ─────────────────────────────────────────
    {
        MarketMakingStrategy::Params p;
        p.half_spread_bps = 3.0;
        p.order_qty       = 0.005;
        p.skew_per_unit   = 0.5;
        p.max_pos_usd     = 1500.0;
        p.requote_ticks   = 5;
        run_backtest(cfg, std::make_shared<MarketMakingStrategy>(p));
    }

    std::cout << "\n[Main] All backtests complete. Reports in ./reports/\n";
    return 0;
}
