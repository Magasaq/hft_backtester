#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// lob_reader.hpp
//
// Reads two CSV files produced by tardis.dev (or compatible format):
//
//   lob.csv   — full limit-order-book snapshots, N levels per side
//               columns: local_timestamp, asks[0].price, asks[0].amount,
//                        bids[0].price, bids[0].amount, asks[1].price, ...
//
//   trades.csv — individual trade prints
//               columns: (index), local_timestamp, side, price, amount
//
// Both timestamp columns are in MICROSECONDS; internally we store nanoseconds.
//
// Output: a single merged, time-sorted stream of hft::Tick objects that the
// existing MarketReplayEngine / BacktestEngine can consume without modification.
//
// The Tick fields are populated as follows:
//   - From LOB row  : bid/ask = best bid/ask (level 0); bid_size/ask_size from level 0
//   - From trade row: last = trade price; volume = trade amount
//   - Between trade rows last_price is carried forward from the most recent trade
// ─────────────────────────────────────────────────────────────────────────────
#include "types.hpp"
#include "data_reader.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include <cmath>

namespace hft {

// ─── Raw LOB snapshot (up to 25 levels per side) ─────────────────────────────
struct LOBSnapshot {
    Timestamp timestamp;   // nanoseconds
    std::vector<Level> bids;  // sorted best-first (descending price)
    std::vector<Level> asks;  // sorted best-first (ascending price)
};

// ─── Raw trade print ──────────────────────────────────────────────────────────
struct TradePrint {
    Timestamp timestamp;
    Side      side;
    Price     price;
    Volume    amount;
};

// ─────────────────────────────────────────────────────────────────────────────
// LOBCSVReader
// ─────────────────────────────────────────────────────────────────────────────
class LOBCSVReader {
public:
    explicit LOBCSVReader(const std::string& path) : path_(path) {}

    std::vector<LOBSnapshot> read_all() {
        std::ifstream f(path_);
        if (!f.is_open()) throw std::runtime_error("Cannot open LOB file: " + path_);

        std::vector<LOBSnapshot> snaps;
        std::string line;
        bool header_done = false;

        // Column index maps  (filled from header)
        int col_ts = -1;
        // ask_price_cols[i] = column index for asks[i].price
        std::vector<int> ask_price_cols, ask_amt_cols;
        std::vector<int> bid_price_cols, bid_amt_cols;

        while (std::getline(f, line)) {
            if (line.empty()) continue;

            if (!header_done) {
                header_done = true;
                auto cols = split(line, ',');
                for (int i = 0; i < (int)cols.size(); ++i) {
                    auto c = trim(cols[i]);
                    if (c == "local_timestamp" || c == "timestamp") {
                        col_ts = i;
                    } else {
                        // asks[N].price / asks[N].amount
                        // bids[N].price / bids[N].amount
                        auto [side, level, field] = parse_col_name(c);
                        if (field == "price") {
                            if (side == "asks") {
                                if ((int)ask_price_cols.size() <= level)
                                    ask_price_cols.resize(level + 1, -1);
                                ask_price_cols[level] = i;
                            } else if (side == "bids") {
                                if ((int)bid_price_cols.size() <= level)
                                    bid_price_cols.resize(level + 1, -1);
                                bid_price_cols[level] = i;
                            }
                        } else if (field == "amount") {
                            if (side == "asks") {
                                if ((int)ask_amt_cols.size() <= level)
                                    ask_amt_cols.resize(level + 1, -1);
                                ask_amt_cols[level] = i;
                            } else if (side == "bids") {
                                if ((int)bid_amt_cols.size() <= level)
                                    bid_amt_cols.resize(level + 1, -1);
                                bid_amt_cols[level] = i;
                            }
                        }
                    }
                }
                if (col_ts < 0) {
                    // First column might be unlabelled index; try col 1
                    col_ts = 1;
                }
                continue;
            }

            auto cols = split(line, ',');
            if ((int)cols.size() < 2) continue;

            LOBSnapshot snap;
            // Timestamps in microseconds → nanoseconds
            snap.timestamp = safe_ll(cols, col_ts) * 1000LL;

            int n_levels = (int)std::min(ask_price_cols.size(), ask_amt_cols.size());
            for (int i = 0; i < n_levels; ++i) {
                double px = safe_d(cols, ask_price_cols[i]);
                double am = safe_d(cols, ask_amt_cols[i]);
                if (px > 0) snap.asks.push_back({px, am});
            }
            n_levels = (int)std::min(bid_price_cols.size(), bid_amt_cols.size());
            for (int i = 0; i < n_levels; ++i) {
                double px = safe_d(cols, bid_price_cols[i]);
                double am = safe_d(cols, bid_amt_cols[i]);
                if (px > 0) snap.bids.push_back({px, am});
            }

            if (!snap.asks.empty() || !snap.bids.empty())
                snaps.push_back(std::move(snap));
        }

        std::sort(snaps.begin(), snaps.end(),
            [](const LOBSnapshot& a, const LOBSnapshot& b){
                return a.timestamp < b.timestamp; });

        std::cout << "[LOBReader] Loaded " << snaps.size()
                  << " snapshots from " << path_ << "\n";
        return snaps;
    }

private:
    std::string path_;

    // Parse "asks[3].price" → ("asks", 3, "price")
    static std::tuple<std::string,int,std::string>
    parse_col_name(const std::string& c) {
        // Format: side[level].field
        auto bracket = c.find('[');
        auto rbracket = c.find(']');
        auto dot = c.find('.');
        if (bracket == std::string::npos || rbracket == std::string::npos)
            return {"", -1, ""};
        std::string side  = c.substr(0, bracket);
        int level = std::stoi(c.substr(bracket + 1, rbracket - bracket - 1));
        std::string field = dot != std::string::npos ? c.substr(dot + 1) : "";
        return {side, level, field};
    }

    static std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> v;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, d)) v.push_back(item);
        return v;
    }
    static std::string trim(std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r\n\""));
        auto p = s.find_last_not_of(" \t\r\n\"");
        if (p != std::string::npos) s.erase(p + 1);
        return s;
    }
    static double safe_d(const std::vector<std::string>& cols, int idx) {
        if (idx < 0 || idx >= (int)cols.size() || cols[idx].empty()) return 0.0;
        try { return std::stod(cols[idx]); } catch (...) { return 0.0; }
    }
    static int64_t safe_ll(const std::vector<std::string>& cols, int idx) {
        if (idx < 0 || idx >= (int)cols.size() || cols[idx].empty()) return 0;
        try { return std::stoll(cols[idx]); } catch (...) { return 0; }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TradesCSVReader
// ─────────────────────────────────────────────────────────────────────────────
class TradesCSVReader {
public:
    explicit TradesCSVReader(const std::string& path) : path_(path) {}

    std::vector<TradePrint> read_all() {
        std::ifstream f(path_);
        if (!f.is_open()) throw std::runtime_error("Cannot open trades file: " + path_);

        std::vector<TradePrint> trades;
        std::string line;
        bool header_done = false;
        int col_ts = -1, col_side = -1, col_price = -1, col_amt = -1;

        while (std::getline(f, line)) {
            if (line.empty()) continue;

            if (!header_done) {
                header_done = true;
                auto cols = split(line, ',');
                for (int i = 0; i < (int)cols.size(); ++i) {
                    auto c = lower(trim(cols[i]));
                    if (c == "local_timestamp" || c == "timestamp") col_ts    = i;
                    else if (c == "side")                            col_side  = i;
                    else if (c == "price")                           col_price = i;
                    else if (c == "amount" || c == "qty" || c == "size") col_amt = i;
                }
                // Fallback if not found by name — assume positional
                // (index), local_timestamp, side, price, amount
                if (col_ts    < 0) col_ts    = 1;
                if (col_side  < 0) col_side  = 2;
                if (col_price < 0) col_price = 3;
                if (col_amt   < 0) col_amt   = 4;
                continue;
            }

            auto cols = split(line, ',');
            if ((int)cols.size() < 4) continue;

            TradePrint tp;
            tp.timestamp = safe_ll(cols, col_ts) * 1000LL;  // µs → ns
            tp.price     = safe_d(cols, col_price);
            tp.amount    = safe_d(cols, col_amt);

            std::string side_str = lower(trim(safe_s(cols, col_side)));
            tp.side = (side_str == "buy") ? Side::BUY : Side::SELL;

            if (tp.price > 0 && tp.amount > 0)
                trades.push_back(tp);
        }

        std::sort(trades.begin(), trades.end(),
            [](const TradePrint& a, const TradePrint& b){
                return a.timestamp < b.timestamp; });

        std::cout << "[TradesReader] Loaded " << trades.size()
                  << " trades from " << path_ << "\n";
        return trades;
    }

private:
    std::string path_;

    static std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> v;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, d)) v.push_back(item);
        return v;
    }
    static std::string trim(std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r\n\""));
        auto p = s.find_last_not_of(" \t\r\n\"");
        if (p != std::string::npos) s.erase(p + 1);
        return s;
    }
    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }
    static double safe_d(const std::vector<std::string>& cols, int idx) {
        if (idx < 0 || idx >= (int)cols.size() || cols[idx].empty()) return 0.0;
        try { return std::stod(cols[idx]); } catch (...) { return 0.0; }
    }
    static int64_t safe_ll(const std::vector<std::string>& cols, int idx) {
        if (idx < 0 || idx >= (int)cols.size() || cols[idx].empty()) return 0;
        try { return std::stoll(cols[idx]); } catch (...) { return 0; }
    }
    static std::string safe_s(const std::vector<std::string>& cols, int idx) {
        if (idx < 0 || idx >= (int)cols.size()) return "";
        return cols[idx];
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// LOBFeedMerger
//
// Merges the LOB snapshot stream and the trades stream into a single
// time-sorted vector<Tick> suitable for MarketReplayEngine::load_ticks().
//
// For each merged event:
//   - LOB snapshot → Tick with bid/ask from level-0, bid_size/ask_size,
//     last = carried forward from last trade
//   - Trade print  → Tick with last = trade price, volume = amount,
//     bid/ask from the most recent LOB snapshot
// ─────────────────────────────────────────────────────────────────────────────
class LOBFeedMerger {
public:
    struct Config {
        std::string symbol   = "UNKNOWN";
        bool        lob_only = false;   // skip trade ticks, only emit LOB ticks
        bool        verbose  = false;
        Config() = default;
    };

    static std::vector<Tick> merge(
        const std::vector<LOBSnapshot>& lobs,
        const std::vector<TradePrint>&  trades,
        const Config& cfg)
    {
        std::vector<Tick> out;
        out.reserve(lobs.size() + trades.size());

        // Interleave by timestamp using two-pointer merge
        size_t li = 0, ti = 0;

        // State carried forward
        Price last_price = 0.0;
        Price last_bid   = 0.0, last_ask = 0.0;
        Volume last_bid_sz = 0.0, last_ask_sz = 0.0;

        auto emit_lob = [&](const LOBSnapshot& s) {
            if (!s.bids.empty()) {
                last_bid    = s.bids[0].price;
                last_bid_sz = s.bids[0].size;
            }
            if (!s.asks.empty()) {
                last_ask    = s.asks[0].price;
                last_ask_sz = s.asks[0].size;
            }
            Tick t;
            t.timestamp = s.timestamp;
            t.symbol    = cfg.symbol;
            t.bid       = last_bid;
            t.ask       = last_ask;
            t.bid_size  = last_bid_sz;
            t.ask_size  = last_ask_sz;
            t.last      = last_price > 0 ? last_price : (last_bid + last_ask) / 2.0;
            t.volume    = 0.0;
            out.push_back(t);
        };

        auto emit_trade = [&](const TradePrint& tp) {
            last_price = tp.price;
            if (cfg.lob_only) return;
            Tick t;
            t.timestamp = tp.timestamp;
            t.symbol    = cfg.symbol;
            t.bid       = last_bid;
            t.ask       = last_ask;
            t.bid_size  = last_bid_sz;
            t.ask_size  = last_ask_sz;
            t.last      = tp.price;
            t.volume    = tp.amount;
            out.push_back(t);
        };

        while (li < lobs.size() && ti < trades.size()) {
            if (lobs[li].timestamp <= trades[ti].timestamp)
                emit_lob(lobs[li++]);
            else
                emit_trade(trades[ti++]);
        }
        while (li < lobs.size())  emit_lob(lobs[li++]);
        while (ti < trades.size()) emit_trade(trades[ti++]);

        // Final sort (should already be sorted, but just in case)
        std::sort(out.begin(), out.end(),
            [](const Tick& a, const Tick& b){ return a.timestamp < b.timestamp; });

        if (cfg.verbose) {
            std::cout << "[LOBMerger] Merged " << out.size()
                      << " ticks (" << lobs.size() << " LOB + "
                      << trades.size() << " trades)\n";
        }
        return out;
    }

    static std::vector<Tick> merge(
        const std::vector<LOBSnapshot>& lobs,
        const std::vector<TradePrint>&  trades)
    {
        Config cfg;
        return merge(lobs, trades, cfg);
    }

    // Convenience: load and merge from file paths
    static std::vector<Tick> from_files(
        const std::string& lob_path,
        const std::string& trades_path,
        const std::string& symbol,
        bool lob_only = false)
    {
        if (is_json(lob_path)) {
            // JSON files already contain merged Tick data — load directly
            JSONDataReader reader(lob_path, symbol);
            auto ticks = reader.read_all();
            std::cout << "[LOBMerger] Merged " << ticks.size()
                      << " ticks (JSON source)\n";
            return ticks;
        }

        LOBCSVReader   lob_reader(lob_path);
        TradesCSVReader tr_reader(trades_path);

        auto lobs   = lob_reader.read_all();
        auto trades = tr_reader.read_all();

        Config cfg;
        cfg.symbol   = symbol;
        cfg.lob_only = lob_only;
        return merge(lobs, trades, cfg);
    }

    // LOB-only (no trades file)
    static std::vector<Tick> from_lob_only(
        const std::string& lob_path,
        const std::string& symbol)
    {
        if (is_json(lob_path)) {
            JSONDataReader reader(lob_path, symbol);
            auto ticks = reader.read_all();
            std::cout << "[LOBMerger] Merged " << ticks.size()
                      << " ticks (JSON source)\n";
            return ticks;
        }

        LOBCSVReader lob_reader(lob_path);
        auto lobs = lob_reader.read_all();
        Config cfg;
        cfg.symbol   = symbol;
        cfg.lob_only = true;
        return merge(lobs, {}, cfg);
    }

    static bool is_json(const std::string& path) {
        if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") return true;
        if (path.size() >= 6 && path.substr(path.size() - 6) == ".jsonl") return true;
        return false;
    }
};

} // namespace hft
