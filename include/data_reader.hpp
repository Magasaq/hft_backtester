#pragma once
#include "types.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <algorithm>

namespace hft {

class CSVDataReader {
public:
    explicit CSVDataReader(const std::string& path) : path_(path) {}

    std::vector<Tick> read_all() {
        std::ifstream file(path_);
        if (!file.is_open())
            throw std::runtime_error("Cannot open data file: " + path_);

        std::vector<Tick> ticks;
        std::string line;
        bool header_done = false;

        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (!header_done) {
                header_done = true;
                if (line.find("timestamp") != std::string::npos ||
                    line.find("time") != std::string::npos) {
                    parse_header(line);
                    continue;   // skip the header row itself
                }
            }
            Tick t = parse_line(line);
            if (t.timestamp > 0) ticks.push_back(t);
        }

        std::sort(ticks.begin(), ticks.end(),
            [](const Tick& a, const Tick& b){ return a.timestamp < b.timestamp; });
        return ticks;
    }

private:
    std::string path_;
    int col_ts_=0,col_sym_=1,col_bid_=2,col_ask_=3,
        col_last_=4,col_bsz_=5,col_asz_=6,col_vol_=7;

    void parse_header(const std::string& hdr) {
        auto cols = split(hdr, ',');
        for (int i = 0; i < (int)cols.size(); ++i) {
            auto c = lower(trim(cols[i]));
            if (c=="timestamp_ns"||c=="timestamp"||c=="time"||c=="ts") col_ts_=i;
            else if (c=="symbol"||c=="sym")               col_sym_=i;
            else if (c=="bid")                             col_bid_=i;
            else if (c=="ask")                             col_ask_=i;
            else if (c=="last"||c=="price"||c=="close")   col_last_=i;
            else if (c=="bid_size"||c=="bidsize")          col_bsz_=i;
            else if (c=="ask_size"||c=="asksize")          col_asz_=i;
            else if (c=="volume"||c=="vol")                col_vol_=i;
        }
    }

    Tick parse_line(const std::string& line) const {
        auto cols = split(line, ',');
        int n = (int)cols.size();
        auto safe_d = [&](int idx, double def=0.0)->double {
            if(idx<0||idx>=n||cols[idx].empty()) return def;
            try{ return std::stod(cols[idx]); } catch(...){ return def; }
        };
        auto safe_s = [&](int idx, const std::string& def="")->std::string {
            if(idx<0||idx>=n) return def; return trim(cols[idx]);
        };
        Tick t;
        t.timestamp = (Timestamp)safe_d(col_ts_);
        t.symbol    = safe_s(col_sym_, "UNKNOWN");
        t.bid       = safe_d(col_bid_);
        t.ask       = safe_d(col_ask_);
        t.last      = safe_d(col_last_);
        t.bid_size  = safe_d(col_bsz_, 1.0);
        t.ask_size  = safe_d(col_asz_, 1.0);
        t.volume    = safe_d(col_vol_);
        if (t.last==0.0 && t.bid>0 && t.ask>0) t.last = (t.bid+t.ask)/2.0;
        if (t.bid==0.0  && t.last>0) { t.bid=t.last*0.9999; t.ask=t.last*1.0001; }
        return t;
    }

    static std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> v; std::stringstream ss(s); std::string item;
        while (std::getline(ss, item, d)) v.push_back(item); return v;
    }
    static std::string trim(std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r\n\""));
        auto p=s.find_last_not_of(" \t\r\n\"");
        if(p!=std::string::npos) s.erase(p+1); return s;
    }
    static std::string lower(std::string s) {
        std::transform(s.begin(),s.end(),s.begin(),::tolower); return s;
    }
};

// ─── JSON Reader ──────────────────────────────────────────────────────────────
//
// Reads tick data in two JSON formats (auto-detected):
//   1. JSON array:  [ {"timestamp":..., "bid":..., ...}, ... ]
//   2. JSONL:       one JSON object per line
//
// Supported field names (case-insensitive):
//   timestamp / timestamp_ns / ts / time
//   symbol / sym
//   bid, ask, last / price, bid_size / bidsize, ask_size / asksize, volume / vol
//
class JSONDataReader {
public:
    explicit JSONDataReader(const std::string& path, const std::string& default_sym = "UNKNOWN")
        : path_(path), default_sym_(default_sym) {}

    std::vector<Tick> read_all() {
        std::ifstream file(path_);
        if (!file.is_open())
            throw std::runtime_error("Cannot open JSON file: " + path_);

        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

        std::vector<Tick> ticks;

        // Detect format: starts with '[' → JSON array; otherwise try JSONL
        size_t first = content.find_first_not_of(" \t\r\n");
        if (first != std::string::npos && content[first] == '[') {
            parse_array(content, ticks);
        } else {
            parse_jsonl(content, ticks);
        }

        std::sort(ticks.begin(), ticks.end(),
            [](const Tick& a, const Tick& b){ return a.timestamp < b.timestamp; });

        std::cout << "[JSONReader] Loaded " << ticks.size()
                  << " ticks from " << path_ << "\n";
        return ticks;
    }

    // ── Generate a sample JSON ticks file ─────────────────────────────────
    static void generate_sample(const std::string& path,
                                const std::string& symbol = "BTC-USD",
                                int n = 1000,
                                double start_price = 65000.0)
    {
        std::ofstream f(path);
        if (!f.is_open()) throw std::runtime_error("Cannot write: " + path);
        f << "[\n";
        std::srand(42);
        double price = start_price;
        int64_t ts = 1700000000000000000LL;
        for (int i = 0; i < n; ++i) {
            double r = ((double)std::rand() / RAND_MAX) * 2.0 - 1.0;
            price *= (1.0 + 0.0002 * r);
            double hs  = price * 0.0001;
            double bid = price - hs, ask = price + hs;
            double vol = 0.01 + ((double)std::rand() / RAND_MAX) * 0.5;
            ts += 100000000LL;
            f << std::fixed;
            f << "  {\"timestamp\":" << ts
              << ",\"symbol\":\"" << symbol << "\""
              << ",\"bid\":"      << std::setprecision(2) << bid
              << ",\"ask\":"      << ask
              << ",\"last\":"     << price
              << ",\"bid_size\":" << std::setprecision(4) << vol
              << ",\"ask_size\":" << vol
              << ",\"volume\":"   << vol
              << "}";
            if (i + 1 < n) f << ",";
            f << "\n";
        }
        f << "]\n";
        std::cout << "[JSONGen] Generated " << n << " ticks → " << path << "\n";
    }

private:
    std::string path_, default_sym_;

    // ── Extract a string value for a key from a JSON object string ─────────
    // Handles both  "key": "value"  and  "key": number
    static std::string extract(const std::string& obj, const std::string& key) {
        // Search for "key": (case-insensitive key match)
        std::string lo = lower_str(obj);
        std::string lk = lower_str("\"" + key + "\"");
        size_t pos = lo.find(lk);
        if (pos == std::string::npos) return "";

        pos = obj.find(':', pos + lk.size());
        if (pos == std::string::npos) return "";
        ++pos;

        // Skip whitespace
        while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t')) ++pos;
        if (pos >= obj.size()) return "";

        if (obj[pos] == '"') {
            // String value
            size_t end = obj.find('"', pos + 1);
            if (end == std::string::npos) return "";
            return obj.substr(pos + 1, end - pos - 1);
        } else {
            // Numeric value — read until delimiter
            size_t end = pos;
            while (end < obj.size() && obj[end] != ',' && obj[end] != '}' &&
                   obj[end] != '\n' && obj[end] != ' ') ++end;
            return obj.substr(pos, end - pos);
        }
    }

    static double exd(const std::string& obj, std::initializer_list<const char*> keys) {
        for (auto k : keys) {
            std::string v = extract(obj, k);
            if (!v.empty()) try { return std::stod(v); } catch (...) {}
        }
        return 0.0;
    }
    static int64_t exll(const std::string& obj, std::initializer_list<const char*> keys) {
        for (auto k : keys) {
            std::string v = extract(obj, k);
            if (!v.empty()) try { return std::stoll(v); } catch (...) {}
        }
        return 0;
    }
    static std::string exs(const std::string& obj, std::initializer_list<const char*> keys,
                           const std::string& def) {
        for (auto k : keys) {
            std::string v = extract(obj, k);
            if (!v.empty()) return v;
        }
        return def;
    }

    Tick parse_object(const std::string& obj) const {
        Tick t;
        t.timestamp = exll(obj, {"timestamp_ns","timestamp","ts","time"});
        t.symbol    = exs (obj, {"symbol","sym"}, default_sym_);
        t.bid       = exd (obj, {"bid"});
        t.ask       = exd (obj, {"ask"});
        t.last      = exd (obj, {"last","price","close"});
        t.bid_size  = exd (obj, {"bid_size","bidsize"});
        t.ask_size  = exd (obj, {"ask_size","asksize"});
        t.volume    = exd (obj, {"volume","vol"});
        if (t.bid_size == 0) t.bid_size = 1.0;
        if (t.ask_size == 0) t.ask_size = 1.0;
        if (t.last == 0.0 && t.bid > 0 && t.ask > 0) t.last = (t.bid + t.ask) / 2.0;
        return t;
    }

    void parse_array(const std::string& content, std::vector<Tick>& out) const {
        size_t pos = 0;
        while ((pos = content.find('{', pos)) != std::string::npos) {
            size_t end = find_matching_brace(content, pos);
            if (end == std::string::npos) break;
            std::string obj = content.substr(pos, end - pos + 1);
            Tick t = parse_object(obj);
            if (t.timestamp > 0) out.push_back(t);
            pos = end + 1;
        }
    }

    void parse_jsonl(const std::string& content, std::vector<Tick>& out) const {
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            size_t s = line.find('{');
            size_t e = line.rfind('}');
            if (s == std::string::npos || e == std::string::npos || e < s) continue;
            std::string obj = line.substr(s, e - s + 1);
            Tick t = parse_object(obj);
            if (t.timestamp > 0) out.push_back(t);
        }
    }

    // Find matching closing brace (skipping nested objects)
    static size_t find_matching_brace(const std::string& s, size_t open) {
        int depth = 0;
        bool in_str = false;
        for (size_t i = open; i < s.size(); ++i) {
            if (in_str) {
                if (s[i] == '\\') { ++i; continue; }
                if (s[i] == '"') in_str = false;
            } else {
                if (s[i] == '"') { in_str = true; continue; }
                if (s[i] == '{') ++depth;
                else if (s[i] == '}') { if (--depth == 0) return i; }
            }
        }
        return std::string::npos;
    }

    static std::string lower_str(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }
};

// ─── Binary Reader ────────────────────────────────────────────────────────────
#pragma pack(push,1)
struct BinaryTickRecord { int64_t ts; double bid,ask,last,vol; };
#pragma pack(pop)

class BinaryDataReader {
public:
    BinaryDataReader(const std::string& path, const std::string& sym)
        : path_(path), sym_(sym) {}
    std::vector<Tick> read_all() {
        std::ifstream f(path_, std::ios::binary);
        if(!f.is_open()) throw std::runtime_error("Cannot open: "+path_);
        std::vector<Tick> v; BinaryTickRecord r;
        while(f.read(reinterpret_cast<char*>(&r),sizeof(r))) {
            Tick t; t.timestamp=r.ts; t.symbol=sym_;
            t.bid=r.bid; t.ask=r.ask; t.last=r.last;
            t.volume=r.vol; t.bid_size=1; t.ask_size=1; v.push_back(t);
        }
        return v;
    }
private:
    std::string path_,sym_;
};

// ─── Sample Data Generator ────────────────────────────────────────────────────
class SampleDataGenerator {
public:
    struct Params {
        std::string symbol      = "BTC-USD";
        int         n_ticks     = 50000;
        double      start_price = 65000.0;
        double      volatility  = 0.0002;
        double      drift       = 0.00001;
        double      spread_bps  = 2.0;
        Timestamp   start_ts    = 1700000000000000000LL;
        int64_t     tick_ns     = 100000000LL;
        unsigned    seed        = 42;
        Params() = default;
    };

    static void generate_csv(const std::string& path, const Params& p) {
        std::ofstream f(path);
        if(!f.is_open()) throw std::runtime_error("Cannot write: "+path);
        f << "timestamp_ns,symbol,bid,ask,last,bid_size,ask_size,volume\n";
        std::srand(p.seed);
        double price = p.start_price;
        Timestamp ts = p.start_ts;
        for(int i=0;i<p.n_ticks;++i) {
            double r = ((double)std::rand()/RAND_MAX)*2.0-1.0;
            double shock = (i>0 && i%5000==0) ? r*p.start_price*0.004 : 0.0;
            price *= (1.0+p.drift+p.volatility*r); price+=shock;
            price = std::max(price, p.start_price*0.4);
            double hs   = price*p.spread_bps/20000.0;
            double bid  = price-hs, ask=price+hs;
            double bsz  = 0.1+((double)std::rand()/RAND_MAX)*2.0;
            double asz  = 0.1+((double)std::rand()/RAND_MAX)*2.0;
            double vol  = 0.01+((double)std::rand()/RAND_MAX)*0.5;
            int64_t jit = (int64_t)(((double)std::rand()/RAND_MAX)*10000000);
            ts += p.tick_ns+jit;
            f<<std::fixed; f.precision(0);
            f<<ts<<","<<p.symbol<<","; f.precision(2);
            f<<bid<<","<<ask<<","<<price<<","; f.precision(4);
            f<<bsz<<","<<asz<<","<<vol<<"\n";
        }
        std::cout<<"[DataGen] Generated "<<p.n_ticks<<" ticks → "<<path<<"\n";
    }

    static void generate_csv(const std::string& path) {
        Params p; generate_csv(path, p);
    }
};

} // namespace hft
