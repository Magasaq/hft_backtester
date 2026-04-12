// main_lob.cpp — LOB + trades backtest entry point
#include "include/types.hpp"
#include "include/lob_reader.hpp"
#include "include/backtest_engine.hpp"
#include "strategies/strategies.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <sys/stat.h>

using namespace hft;

struct CLIArgs {
    std::string lob_path;
    std::string trades_path;
    std::string symbol        = "UNKNOWN";
    double      cash          = 100'000.0;
    double      commission    = 1.0;
    double      slippage      = 0.5;
    double      notional      = 500.0;   // target USD per order
    bool        lob_only      = false;
    bool        verbose       = false;
    std::string strategy      = "all";
};

CLIArgs parse_args(int argc, char** argv) {
    CLIArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg=="--lob"        && i+1<argc) a.lob_path    = argv[++i];
        else if (arg=="--trades"     && i+1<argc) a.trades_path = argv[++i];
        else if (arg=="--symbol"     && i+1<argc) a.symbol      = argv[++i];
        else if (arg=="--cash"       && i+1<argc) a.cash        = std::stod(argv[++i]);
        else if (arg=="--commission" && i+1<argc) a.commission  = std::stod(argv[++i]);
        else if (arg=="--slippage"   && i+1<argc) a.slippage    = std::stod(argv[++i]);
        else if (arg=="--notional"   && i+1<argc) a.notional    = std::stod(argv[++i]);
        else if (arg=="--strategy"   && i+1<argc) a.strategy    = argv[++i];
        else if (arg=="--lob-only")               a.lob_only    = true;
        else if (arg=="--verbose")                a.verbose     = true;
    }
    return a;
}

void print_usage(const char* prog) {
    std::cout << "\nUsage:\n"
              << "  " << prog << " --lob lob.csv [--trades trades.csv] [options]\n\n"
              << "Options:\n"
              << "  --lob        <path>   LOB snapshots CSV (required)\n"
              << "  --trades     <path>   Trades CSV        (optional)\n"
              << "  --symbol     <sym>    e.g. XBT-USD      (default: UNKNOWN)\n"
              << "  --cash       <amt>    Starting capital  (default: 100000)\n"
              << "  --notional   <amt>    USD per order     (default: 500)\n"
              << "  --commission <bps>    Per trade, bps    (default: 1.0)\n"
              << "  --slippage   <bps>    Market impact bps (default: 0.5)\n"
              << "  --strategy   <name>   all|meanrev|momentum|mm\n"
              << "  --lob-only            Skip trade ticks\n"
              << "  --verbose             Print every fill\n\n"
              << "Example:\n"
              << "  " << prog
              << " --lob data/lob.csv --trades data/trades.csv"
              << " --symbol XBT-USD --notional 500\n\n";
}

// Compute order qty so each trade ≈ target_notional USD
double qty_for_notional(double price, double target_notional) {
    if (price <= 0) return 1000.0;
    return target_notional / price;
}

template<typename S>
void run_strategy(const std::vector<Tick>& ticks,
                  const CLIArgs& a,
                  std::shared_ptr<S> strat)
{
    BacktestConfig cfg;
    cfg.symbol         = a.symbol;
    cfg.commission_bps = a.commission;
    cfg.slippage_bps   = a.slippage;
    cfg.initial_cash   = a.cash;
    cfg.verbose        = a.verbose;

    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  Strategy  : " << strat->name() << "\n";
    std::cout << "  Symbol    : " << a.symbol << "\n";
    std::cout << "  Ticks     : " << ticks.size() << "\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    BacktestEngine engine(cfg);
    engine.load_ticks(ticks);
    engine.add_strategy(strat);
    engine.run();
}

int main(int argc, char** argv) {
    std::cout << "\n  HFT Backtest — LOB + Trades Mode\n\n";

    auto args = parse_args(argc, argv);
    if (args.lob_path.empty()) { print_usage(argv[0]); return 1; }
    struct stat st;
    if (stat(args.lob_path.c_str(), &st) != 0) {
            std::cerr << "ERROR: LOB file not found: " << args.lob_path << "\n";
        return 1;
    }

    mkdir("reports", 0755);

    // ── Load data ──────────────────────────────────────────────────────────
    std::vector<Tick> ticks;
    if (!args.trades_path.empty() && stat(args.trades_path.c_str(), &st) == 0)
        ticks = LOBFeedMerger::from_files(
            args.lob_path, args.trades_path, args.symbol, args.lob_only);
    else
        ticks = LOBFeedMerger::from_lob_only(args.lob_path, args.symbol);

    if (ticks.empty()) { std::cerr << "ERROR: No ticks.\n"; return 1; }

    // Find a representative mid price from first valid tick
    double ref_price = 0;
    for (const auto& t : ticks) {
        if (t.bid > 0 && t.ask > 0) { ref_price = (t.bid + t.ask) / 2.0; break; }
    }

    double order_qty = qty_for_notional(ref_price, args.notional);

    std::cout << "\n[Data] First tick  : ts=" << ticks.front().timestamp
              << "  bid=" << std::fixed << std::setprecision(8)
              << ticks.front().bid << "  ask=" << ticks.front().ask << "\n";
    std::cout << "[Data] Last tick   : ts=" << ticks.back().timestamp
              << "  bid=" << ticks.back().bid
              << "  ask=" << ticks.back().ask << "\n";
    std::cout << "[Data] Duration    : " << std::setprecision(1)
              << (ticks.back().timestamp - ticks.front().timestamp) / 3.6e12
              << " hours\n";
    std::cout << "[Data] Ref price   : " << std::setprecision(8) << ref_price << "\n";
    std::cout << "[Data] Order qty   : " << std::setprecision(0) << order_qty
              << " units  (~$" << std::setprecision(0) << args.notional
              << " notional)\n";

    bool all = (args.strategy == "all");

    // ── Mean Reversion ─────────────────────────────────────────────────────
    if (all || args.strategy == "meanrev") {
        MeanReversionStrategy::Params p;
        p.lookback       = 50;
        p.num_std        = 1.5;
        p.order_qty      = order_qty;
        p.cooldown_ticks = 3;
        p.max_position   = order_qty * 5;
        p.stop_loss_pct  = 0.008;
        run_strategy(ticks, args, std::make_shared<MeanReversionStrategy>(p));
    }

    // ── Momentum ───────────────────────────────────────────────────────────
    if (all || args.strategy == "momentum") {
        MomentumStrategy::Params p;
        p.short_period     = 10;
        p.long_period      = 30;
        p.order_qty        = order_qty;
        p.entry_offset_bps = 1.0;
        p.max_position     = order_qty * 4;
        run_strategy(ticks, args, std::make_shared<MomentumStrategy>(p));
    }

    // ── Market Making ──────────────────────────────────────────────────────
    if (all || args.strategy == "mm") {
        MarketMakingStrategy::Params p;
        p.half_spread_bps = 3.0;
        p.order_qty       = order_qty / 2.0;     // smaller size, faster turnover
        p.skew_per_unit   = 0.5;
        p.max_pos_usd     = args.notional * 3.0; // stop out at 3x notional
        p.requote_ticks   = 5;
        run_strategy(ticks, args, std::make_shared<MarketMakingStrategy>(p));
    }

    std::cout << "\n[Done] Reports written to ./reports/\n";
    return 0;
}
