# HFT Backtesting Framework — Technical Documentation

## Overview

A modular, high-performance C++17 backtesting framework for evaluating trading strategies
against historical tick-level market data. Designed around a clean separation of concerns:
data ingestion → market replay → exchange emulation → strategy → analytics.

```
┌──────────────┐     ┌──────────────────┐     ┌──────────────────┐
│  Data Layer  │────▶│  Replay Engine   │────▶│ Exchange Emulator│
│ CSV/JSON/Bin │     │  Time-controlled │     │  Price-crossing  │
└──────────────┘     └──────────────────┘     └────────┬─────────┘
                                                        │ fills
                              ┌─────────────────────────▼──────────┐
                              │           Strategy Interface        │
                              │  MeanReversion │ Momentum │ MM      │
                              └─────────────────────────┬──────────┘
                                                        │ trades
                              ┌─────────────────────────▼──────────┐
                              │           PnL Tracker              │
                              │  Equity curve, Drawdown, Sharpe    │
                              └────────────────────────────────────┘
```

---

## Directory Structure

```
hft_backtest/
├── include/
│   ├── types.hpp           # Core data types (Order, Trade, Tick, OrderBook)
│   ├── data_reader.hpp     # CSV, Binary readers + sample data generator
│   ├── market_replay.hpp   # Time-controlled market replay engine
│   ├── exchange.hpp        # Exchange emulator with price-crossing execution
│   ├── strategy.hpp        # Abstract strategy interface
│   ├── pnl_tracker.hpp     # PnL accounting and performance statistics
│   └── backtest_engine.hpp # Top-level orchestrator
├── strategies/
│   └── strategies.hpp      # MeanReversion, Momentum, MarketMaking
├── tests/
│   └── test_main.cpp       # Unit tests (6 test cases)
├── tools/
│   └── gen_data.cpp        # Standalone data generation utility
├── data/
│   └── sample_ticks.csv    # Auto-generated on first run
├── reports/                # CSV output: equity curves, trade logs
├── main.cpp                # Entry point — runs all 3 strategies
└── CMakeLists.txt
```

---

## Build Instructions

**Requirements:** GCC ≥ 9 (or Clang ≥ 10), CMake ≥ 3.16, C++17

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Executables produced:
#   build/hft_backtest  — main backtest runner
#   build/run_tests     — unit test suite
#   build/gen_data      — data generation utility
```

---

## Running

### Full backtest (all 3 strategies):
```bash
./build/hft_backtest
```

### With a custom data file:
```bash
./build/hft_backtest --data /path/to/ticks.csv --symbol ETH-USD --cash 50000
```

### CLI flags:
| Flag           | Default                 | Description                      |
|----------------|-------------------------|----------------------------------|
| `--data`       | `data/sample_ticks.csv` | Path to tick CSV file            |
| `--symbol`     | `BTC-USD`               | Instrument symbol                |
| `--cash`       | `100000`                | Initial capital in USD           |
| `--commission` | `1.0`                   | Commission in basis points       |
| `--verbose`    | off                     | Enable per-tick logging          |

### Unit tests:
```bash
./build/run_tests
```

### Generate a larger dataset:
```bash
./build/gen_data data/ticks_100k.csv 100000
```

---

## Data Format

### CSV (default)
Header must contain (order-independent, case-insensitive):

```
timestamp_ns,symbol,bid,ask,last,bid_size,ask_size,volume
```

- `timestamp_ns` — nanoseconds since Unix epoch (int64)
- `symbol`       — e.g. `BTC-USD`
- `bid`, `ask`   — best bid/ask prices
- `last`         — last trade price (inferred as mid if missing)
- `bid_size`, `ask_size` — top-of-book sizes (optional, default 1.0)
- `volume`       — tick volume (optional)

Missing columns are inferred where possible. The reader detects and skips the header row automatically.

### JSON (array or JSONL)
Two formats are accepted, auto-detected:

**JSON array:**
```json
[
  {"timestamp": 1700000000100000000, "symbol": "BTC-USD", "bid": 64980.5, "ask": 64993.5, "last": 64987.0, "bid_size": 0.4, "ask_size": 0.3, "volume": 0.12},
  ...
]
```

**JSONL (one object per line):**
```
{"timestamp": 1700000000100000000, "bid": 64980.5, "ask": 64993.5, ...}
{"timestamp": 1700000000200000000, "bid": 64981.0, "ask": 64994.0, ...}
```

Field names are case-insensitive. Aliases supported: `timestamp`/`timestamp_ns`/`ts`, `symbol`/`sym`, `last`/`price`, `volume`/`vol`, `bid_size`/`bidsize`, `ask_size`/`asksize`.

Generate a sample JSON file:
```cpp
JSONDataReader::generate_sample("data/sample.json", "BTC-USD", 1000);
```

Or run via CLI:
```bash
./build/hft_lob --lob data/sample.json --symbol BTC-USD --notional 500
```

### Binary
Fixed-record format: `[int64 ts][double bid][double ask][double last][double vol]` = 40 bytes/record.
Use `BinaryDataReader("file.bin", "SYM")`.

---

## Execution Criterion

The exchange fills limit orders using the **price-crossing rule** (as specified):

| Order Side | Fill Condition            | Rationale                              |
|------------|---------------------------|----------------------------------------|
| BUY limit  | `market_ask ≤ order_price` | Market price dropped to our bid level |
| SELL limit | `market_bid ≥ order_price` | Market price rose to our ask level    |

Market orders fill immediately at the prevailing ask (BUY) or bid (SELL) with configurable slippage.

---

## Component Reference

### `types.hpp`
Core primitives used throughout:
- `Tick` — a single market data snapshot (ts, symbol, bid, ask, last, sizes, volume)
- `Order` — submitted order with id, side, type, price, qty, status
- `Trade` — an execution fill (price, qty, ts, order id)
- `OrderBook` — multi-level bid/ask snapshot
- `BacktestConfig` — top-level simulation parameters

### `CSVDataReader`
```cpp
CSVDataReader reader("data/ticks.csv");
std::vector<Tick> ticks = reader.read_all();  // loads & sorts all ticks
```

### `MarketReplayEngine`
```cpp
MarketReplayEngine::Config cfg;
cfg.start_ts = 0; cfg.end_ts = 0; cfg.book_depth = 5;
MarketReplayEngine replay(cfg);
replay.load_csv("data/ticks.csv");
replay.on_tick([](const Tick& t) { /* handle */ });
replay.on_end([]() { /* cleanup */ });
replay.run();  // processes all ticks in timestamp order
```

### `ExchangeEmulator`
```cpp
ExchangeEmulator::Config cfg;
cfg.commission_bps = 1.0; cfg.slippage_bps = 0.5;
ExchangeEmulator ex(cfg);

ex.on_fill([](const Trade& t) { /* handle fill */ });

// Submit a limit buy order
Order o; o.symbol="BTC-USD"; o.side=Side::BUY;
o.type=OrderType::LIMIT; o.price=64900.0; o.quantity=0.01;
OrderId id = ex.submit(o);

// Called each tick — checks for price-crossing fills
ex.process_tick(tick);

ex.cancel(id);  // cancel open order
```

### `PnLTracker`
```cpp
PnLTracker tracker(100000.0);        // initial cash
tracker.set_commission_bps(1.0);
tracker.on_fill(trade);              // called on each execution
tracker.on_tick(tick);               // mark-to-market each tick

auto stats = tracker.compute_stats();
tracker.print_report();
tracker.export_equity_csv("reports/equity.csv");
tracker.export_trades_csv("reports/trades.csv");
```

### Writing a Custom Strategy
```cpp
class MyStrategy : public Strategy {
public:
    std::string name() const override { return "MyStrategy"; }

    void on_tick(const Tick& tick) override {
        double mid = (tick.bid + tick.ask) / 2.0;
        // BUY limit 0.1% below mid — fills when price crosses down
        buy(tick.symbol, mid * 0.999, 0.01);
    }

    void on_fill(const Trade& t) override {
        // React to fill — place take-profit, etc.
    }

    void on_end() override {
        // Flatten any open positions at market
    }
};

// Register and run:
BacktestEngine engine(cfg);
engine.add_strategy(std::make_shared<MyStrategy>());
engine.run();
```

---

## Included Strategies

### 1. MeanReversion (Bollinger Bands)
- Computes rolling mean and σ over N ticks
- Places BUY limit at `MA − k·σ` (lower band)
- Places SELL limit at `MA + k·σ` (upper band)
- Cancels and re-quotes as bands shift
- Stop-loss exits on adverse moves > threshold

**Key params:** `lookback=50`, `num_std=1.5`, `order_qty=0.01 BTC`

### 2. Momentum (EMA Crossover)
- Tracks short (10-tick) and long (30-tick) exponential moving averages
- Enters long on bullish EMA cross, short on bearish cross
- Places limit order slightly inside spread for passive entry

**Key params:** `short=10`, `long=30`, `entry_offset_bps=1.0`

### 3. MarketMaking (Inventory-Skewed Quoting)
- Continuously posts bid/ask symmetrically around mid
- Skews quotes based on current inventory to reduce position risk
- Re-quotes every N ticks as price moves

**Key params:** `half_spread_bps=3.0`, `order_qty=0.005 BTC`, `requote_ticks=5`

---

## Performance Reports

After each run, the following are written to `reports/`:

| File                           | Contents                                   |
|--------------------------------|--------------------------------------------|
| `<Strategy>_equity.csv`        | Timestamped equity, PnL, drawdown curve    |
| `<Strategy>_trades.csv`        | Full trade log (entry/exit price, PnL)     |

### Metrics computed:
- **Total Return %**, Net PnL, Gross PnL
- **Max Drawdown %**
- **Sharpe Ratio** (annualised, from equity returns)
- **Sortino Ratio** (downside-deviation adjusted)
- **Profit Factor** = gross wins / gross losses
- **Win Rate %**, Avg Win/Loss, Largest Win/Loss
- Total trades, winning/losing trades
- Avg trade duration (seconds)
- Commission paid

---

## Performance Benchmarks

Tested on Intel i7-class CPU, 50,000 ticks, single strategy:

| Strategy      | Wall Time | Throughput         |
|---------------|-----------|--------------------|
| MeanReversion | ~140ms    | ~358,000 ticks/sec |
| Momentum      | ~111ms    | ~451,000 ticks/sec |
| MarketMaking  | ~97ms     | ~514,000 ticks/sec |

All strategies process 50K ticks in under 150ms, well within real-time constraints for HFT simulation workloads. The architecture supports scaling to millions of ticks without architectural changes.

---

## Extending the Framework

### Adding a new data format
Implement a reader that produces `std::vector<Tick>` and call `replay.load_ticks(ticks)`.

### Adding new statistics
Extend `PerformanceStats` in `pnl_tracker.hpp` and add computation to `compute_stats()`.

### Multi-symbol support
Each `BacktestEngine` instance handles one symbol. Run multiple instances in parallel threads for portfolio-level backtesting.

### Connecting to a real feed
Replace `MarketReplayEngine::run()` with a live tick feed that calls the same `TickHandler` callbacks — the exchange and strategies require no modification.
