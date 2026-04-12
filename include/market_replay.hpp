#pragma once
#include "types.hpp"
#include "data_reader.hpp"
#include <queue>
#include <optional>
#include <functional>
#include <memory>
#include <iostream>

namespace hft {

// ─── Event types the replay engine emits ─────────────────────────────────────
enum class EventType { TICK, ORDER_BOOK, HEARTBEAT, END_OF_DATA };

struct ReplayEvent {
    Timestamp   timestamp;
    EventType   type;
    std::optional<Tick>       tick;
    std::optional<OrderBook>  book;

    bool operator>(const ReplayEvent& o) const { return timestamp > o.timestamp; }
};

// ─── Callback signatures ──────────────────────────────────────────────────────
using TickHandler      = std::function<void(const Tick&)>;
using OrderBookHandler = std::function<void(const OrderBook&)>;
using EODHandler       = std::function<void()>;

// ─── Market Replay Engine ─────────────────────────────────────────────────────
//
// Replays a sorted stream of market ticks in order, optionally filtered by
// time window. Builds an order-book snapshot from tick data and emits both
// raw ticks and synthesised order books.
//
class MarketReplayEngine {
public:
    struct Config {
        Config() = default;
        Timestamp start_ts   = 0;      // 0 = from beginning
        Timestamp end_ts     = 0;      // 0 = to end
        int       book_depth = 5;      // levels to synthesise per side
        bool      verbose    = false;
        int       heartbeat_ms = 0;    // 0 = disabled
    };

    MarketReplayEngine() {}
    explicit MarketReplayEngine(Config cfg) : cfg_(cfg) {}

    // ── Register handlers ──────────────────────────────────────────────────
    void on_tick(TickHandler h)            { tick_handlers_.push_back(std::move(h)); }
    void on_order_book(OrderBookHandler h) { book_handlers_.push_back(std::move(h)); }
    void on_end(EODHandler h)              { eod_handlers_.push_back(std::move(h)); }

    // ── Load data ──────────────────────────────────────────────────────────
    void load_ticks(std::vector<Tick> ticks) {
        ticks_ = std::move(ticks);
        apply_time_filter();
        index_ = 0;
        if (cfg_.verbose)
            std::cout << "[Replay] Loaded " << ticks_.size() << " ticks\n";
    }

    void load_csv(const std::string& path) {
        CSVDataReader reader(path);
        load_ticks(reader.read_all());
    }

    void load_json(const std::string& path, const std::string& symbol = "UNKNOWN") {
        JSONDataReader reader(path, symbol);
        load_ticks(reader.read_all());
    }

    // Auto-detect format from file extension
    void load_file(const std::string& path, const std::string& symbol = "UNKNOWN") {
        if (path.size() >= 5 &&
            (path.substr(path.size()-5) == ".json" ||
             path.substr(path.size()-6) == ".jsonl")) {
            load_json(path, symbol);
        } else {
            load_csv(path);
        }
    }

    // ── Run the replay ─────────────────────────────────────────────────────
    void run() {
        if (ticks_.empty()) {
            std::cerr << "[Replay] No ticks loaded.\n";
            return;
        }

        stats_ = {};
        current_ts_ = ticks_.front().timestamp;

        for (const auto& tick : ticks_) {
            if (cfg_.end_ts > 0 && tick.timestamp > cfg_.end_ts) break;

            current_ts_ = tick.timestamp;
            ++stats_.ticks_processed;

            // Emit tick
            for (auto& h : tick_handlers_) h(tick);

            // Update internal book and emit snapshot
            update_book(tick);
            if (!book_handlers_.empty()) {
                auto snap = build_snapshot(tick.symbol);
                for (auto& h : book_handlers_) h(snap);
            }
        }

        // End of data
        for (auto& h : eod_handlers_) h();

        if (cfg_.verbose) {
            std::cout << "[Replay] Done. " << stats_.ticks_processed
                      << " ticks replayed over "
                      << (current_ts_ - ticks_.front().timestamp) / 1e9
                      << "s\n";
        }
    }

    // ── Step-by-step mode (for interactive strategies) ─────────────────────
    bool step() {
        if (index_ >= ticks_.size()) return false;
        const auto& tick = ticks_[index_++];
        current_ts_ = tick.timestamp;
        ++stats_.ticks_processed;
        for (auto& h : tick_handlers_) h(tick);
        update_book(tick);
        if (!book_handlers_.empty()) {
            auto snap = build_snapshot(tick.symbol);
            for (auto& h : book_handlers_) h(snap);
        }
        return true;
    }

    bool at_end() const { return index_ >= ticks_.size(); }

    // ── Accessors ──────────────────────────────────────────────────────────
    Timestamp current_time()  const { return current_ts_; }
    size_t    ticks_loaded()  const { return ticks_.size(); }
    double    progress_pct()  const {
        if (ticks_.empty()) return 0.0;
        return 100.0 * index_ / ticks_.size();
    }

    struct Stats {
        size_t ticks_processed = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    Config cfg_;
    std::vector<Tick> ticks_;
    size_t index_ = 0;
    Timestamp current_ts_ = 0;
    Stats stats_;

    // Per-symbol last known bid/ask used to synthesise order book
    struct BookState {
        Price bid = 0, ask = 0;
        Volume bid_sz = 0, ask_sz = 0;
    };
    std::unordered_map<std::string, BookState> book_states_;

    std::vector<TickHandler>      tick_handlers_;
    std::vector<OrderBookHandler> book_handlers_;
    std::vector<EODHandler>       eod_handlers_;

    void apply_time_filter() {
        if (cfg_.start_ts > 0) {
            ticks_.erase(
                std::remove_if(ticks_.begin(), ticks_.end(),
                    [&](const Tick& t){ return t.timestamp < cfg_.start_ts; }),
                ticks_.end());
        }
    }

    void update_book(const Tick& t) {
        auto& bs = book_states_[t.symbol];
        if (t.bid > 0) { bs.bid = t.bid; bs.bid_sz = t.bid_size; }
        if (t.ask > 0) { bs.ask = t.ask; bs.ask_sz = t.ask_size; }
    }

    // Synthesise a multi-level order book from tick data
    OrderBook build_snapshot(const std::string& sym) const {
        OrderBook ob;
        ob.symbol    = sym;
        ob.timestamp = current_ts_;

        auto it = book_states_.find(sym);
        if (it == book_states_.end()) return ob;

        const auto& bs = it->second;
        int depth = cfg_.book_depth;

        // Build synthetic levels with decaying sizes
        for (int i = 0; i < depth; ++i) {
            double tick_size = bs.bid * 0.0001;  // 1bp tick
            ob.bids.push_back({ bs.bid - i * tick_size,
                                bs.bid_sz * (1.0 - 0.1 * i) });
            ob.asks.push_back({ bs.ask + i * tick_size,
                                bs.ask_sz * (1.0 - 0.1 * i) });
        }
        return ob;
    }
};

} // namespace hft
