#pragma once
#include <cstdint>
#include <string>
#include <chrono>
#include <variant>
#include <optional>
#include <vector>
#include <unordered_map>

namespace hft {

// ─── Timestamp ───────────────────────────────────────────────────────────────
using Timestamp = int64_t;  // nanoseconds since epoch

inline Timestamp now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline std::string ts_to_str(Timestamp ts) {
    auto ms = ts / 1'000'000;
    auto sec = ms / 1000;
    auto ms_part = ms % 1000;
    std::time_t t = sec;
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::gmtime(&t));
    return std::string(buf) + "." + std::to_string(ms_part);
}

// ─── Price & Size ─────────────────────────────────────────────────────────────
using Price  = double;
using Volume = double;
using OrderId = uint64_t;

// ─── Side ─────────────────────────────────────────────────────────────────────
enum class Side : uint8_t { BUY = 0, SELL = 1 };
inline Side opposite(Side s) { return s == Side::BUY ? Side::SELL : Side::BUY; }
inline std::string side_str(Side s) { return s == Side::BUY ? "BUY" : "SELL"; }

// ─── Order Types ──────────────────────────────────────────────────────────────
enum class OrderType : uint8_t { LIMIT, MARKET };
enum class OrderStatus : uint8_t { PENDING, OPEN, PARTIAL, FILLED, CANCELLED };

// ─── Order ────────────────────────────────────────────────────────────────────
struct Order {
    OrderId    id;
    std::string symbol;
    Side       side;
    OrderType  type;
    Price      price;           // limit price (0 for market)
    Volume     quantity;
    Volume     filled_qty  = 0.0;
    OrderStatus status     = OrderStatus::PENDING;
    Timestamp  created_at  = 0;
    Timestamp  updated_at  = 0;
    std::string strategy_id;

    Volume remaining() const { return quantity - filled_qty; }
    bool   is_complete() const {
        return status == OrderStatus::FILLED || status == OrderStatus::CANCELLED;
    }
};

// ─── Trade (fill) ─────────────────────────────────────────────────────────────
struct Trade {
    OrderId     order_id   = 0;
    std::string symbol;
    Side        side       = Side::BUY;
    Price       price      = 0;
    Volume      quantity   = 0;
    Timestamp   timestamp  = 0;
    std::string strategy_id;
};

// ─── Market Tick (raw market data row) ───────────────────────────────────────
struct Tick {
    Timestamp timestamp;
    std::string symbol;
    Price      bid;
    Price      ask;
    Price      last;       // last trade price
    Volume     bid_size;
    Volume     ask_size;
    Volume     volume;     // tick volume
};

// ─── Order Book Level ─────────────────────────────────────────────────────────
struct Level {
    Price  price;
    Volume size;
};

// ─── Order Book Snapshot ──────────────────────────────────────────────────────
struct OrderBook {
    std::string symbol;
    Timestamp   timestamp;
    std::vector<Level> bids;   // sorted descending
    std::vector<Level> asks;   // sorted ascending

    Price best_bid() const { return bids.empty() ? 0.0 : bids[0].price; }
    Price best_ask() const { return asks.empty() ? 0.0 : asks[0].price; }
    Price mid()      const { return (best_bid() + best_ask()) / 2.0; }
    Price spread()   const { return best_ask() - best_bid(); }
};

// ─── Market Events (variant for replay queue) ────────────────────────────────
using MarketEvent = std::variant<Tick, OrderBook>;

// ─── Backtest Config ──────────────────────────────────────────────────────────
struct BacktestConfig {
    std::string  data_file;
    std::string  symbol         = "BTC-USD";
    std::string  data_format    = "csv";
    double       commission_bps = 1.0;
    double       slippage_bps   = 0.5;
    double       initial_cash   = 100'000.0;
    bool         verbose        = false;
    Timestamp    start_ts       = 0;
    Timestamp    end_ts         = 0;
    BacktestConfig() = default;
};

} // namespace hft
