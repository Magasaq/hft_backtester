#include "../include/types.hpp"
#include "../include/exchange.hpp"
#include "../include/pnl_tracker.hpp"
#include "../include/data_reader.hpp"
#include <cassert>
#include <iostream>
#include <sstream>

using namespace hft;

// ── Simple test framework ─────────────────────────────────────────────────────
static int tests_run = 0, tests_passed = 0;
#define TEST(name) \
    std::cout << "  TEST: " << #name << " ... "; \
    try { test_##name(); ++tests_passed; std::cout << "PASS\n"; } \
    catch(const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; } \
    ++tests_run;

#define ASSERT(cond) \
    if(!(cond)) throw std::runtime_error("Assertion failed: " #cond)
#define ASSERT_NEAR(a,b,eps) \
    if(std::abs((a)-(b)) > (eps)) \
        throw std::runtime_error("Expected " #a " ≈ " #b " but got " + std::to_string(a))

// ─────────────────────────────────────────────────────────────────────────────

void test_buy_fill_on_ask_cross() {
    // BUY limit @ 100 should fill when ask drops to 100
    ExchangeEmulator ex;
    std::vector<Trade> fills;
    ex.on_fill([&](const Trade& t){ fills.push_back(t); });

    // Seed price
    Tick t1; t1.timestamp=1; t1.symbol="X"; t1.bid=101; t1.ask=102; t1.last=101.5;
    ex.process_tick(t1);

    Order o; o.symbol="X"; o.side=Side::BUY; o.type=OrderType::LIMIT;
    o.price=100.5; o.quantity=1.0; o.strategy_id="test";
    ex.submit(o);

    ASSERT(fills.empty());  // shouldn't fill yet (ask=102 > 100.5)

    // Tick where ask crosses order price
    Tick t2; t2.timestamp=2; t2.symbol="X"; t2.bid=100.0; t2.ask=100.4; t2.last=100.2;
    ex.process_tick(t2);

    ASSERT(fills.size() == 1);
    ASSERT(fills[0].side == Side::BUY);
    ASSERT_NEAR(fills[0].price, 100.5, 0.001);
}

void test_sell_fill_on_bid_cross() {
    ExchangeEmulator ex;
    std::vector<Trade> fills;
    ex.on_fill([&](const Trade& t){ fills.push_back(t); });

    Tick t1; t1.timestamp=1; t1.symbol="X"; t1.bid=98; t1.ask=99; t1.last=98.5;
    ex.process_tick(t1);

    Order o; o.symbol="X"; o.side=Side::SELL; o.type=OrderType::LIMIT;
    o.price=100.0; o.quantity=0.5; o.strategy_id="test";
    ex.submit(o);

    ASSERT(fills.empty()); // bid=98 < 100

    Tick t2; t2.timestamp=2; t2.symbol="X"; t2.bid=100.5; t2.ask=101.0; t2.last=100.7;
    ex.process_tick(t2);

    ASSERT(fills.size() == 1);
    ASSERT(fills[0].side == Side::SELL);
}

void test_cancel_removes_order() {
    ExchangeEmulator ex;
    Tick t1; t1.timestamp=1; t1.symbol="X"; t1.bid=100; t1.ask=101; t1.last=100.5;
    ex.process_tick(t1);

    Order o; o.symbol="X"; o.side=Side::BUY; o.type=OrderType::LIMIT;
    o.price=99; o.quantity=1; o.strategy_id="test";
    auto id = ex.submit(o);

    ASSERT(ex.open_order_count() == 1);
    ex.cancel(id);
    ASSERT(ex.open_order_count() == 0);

    // Should not fill after cancel
    std::vector<Trade> fills;
    ex.on_fill([&](const Trade& t){ fills.push_back(t); });
    Tick t2; t2.timestamp=2; t2.symbol="X"; t2.bid=97; t2.ask=98; t2.last=97.5;
    ex.process_tick(t2);
    ASSERT(fills.empty());
}

void test_pnl_tracker_cash_accounting() {
    PnLTracker tracker(10000.0);
    tracker.set_commission_bps(0.0); // zero commission for simplicity

    Trade buy_trade;
    buy_trade.order_id=1; buy_trade.symbol="X";
    buy_trade.side=Side::BUY; buy_trade.price=100; buy_trade.quantity=1;
    buy_trade.timestamp=1; buy_trade.strategy_id="t";
    tracker.on_fill(buy_trade);

    ASSERT_NEAR(tracker.position(), 1.0, 1e-6);
    ASSERT_NEAR(tracker.cash(),  9900.0, 0.01);

    // Mark to market at 110
    Tick t; t.timestamp=2; t.symbol="X"; t.bid=109; t.ask=111; t.last=110;
    tracker.on_tick(t);
    ASSERT_NEAR(tracker.equity(), 10010.0, 0.01); // 9900 cash + 110 position

    Trade sell_trade;
    sell_trade.order_id=2; sell_trade.symbol="X";
    sell_trade.side=Side::SELL; sell_trade.price=110; sell_trade.quantity=1;
    sell_trade.timestamp=3; sell_trade.strategy_id="t";
    tracker.on_fill(sell_trade);

    ASSERT_NEAR(tracker.cash(), 10010.0, 0.01);
    ASSERT_NEAR(tracker.position(), 0.0, 1e-6);
}

void test_csv_reader_parse() {
    // Write a tiny CSV to memory (via temp file)
    const char* path = "/tmp/test_ticks.csv";
    {
        std::ofstream f(path);
        f << "timestamp_ns,symbol,bid,ask,last,bid_size,ask_size,volume\n";
        f << "1000000000,BTC-USD,65000.00,65002.00,65001.00,1.0,1.0,0.5\n";
        f << "2000000000,BTC-USD,65010.00,65012.00,65011.00,0.8,1.2,0.3\n";
    }
    CSVDataReader reader(path);
    auto ticks = reader.read_all();
    ASSERT(ticks.size() == 2);
    ASSERT_NEAR(ticks[0].bid, 65000.0, 0.01);
    ASSERT_NEAR(ticks[1].ask, 65012.0, 0.01);
    ASSERT(ticks[0].symbol == "BTC-USD");
}

void test_market_order_fills_immediately() {
    ExchangeEmulator ex;
    std::vector<Trade> fills;
    ex.on_fill([&](const Trade& t){ fills.push_back(t); });

    // Seed prices
    Tick t1; t1.timestamp=1; t1.symbol="X"; t1.bid=100; t1.ask=101; t1.last=100.5;
    ex.process_tick(t1);

    Order o; o.symbol="X"; o.side=Side::BUY; o.type=OrderType::MARKET;
    o.quantity=1.0; o.strategy_id="test";
    ex.submit(o);

    ASSERT(fills.size() == 1); // Market order fills immediately
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n╔══════════════════════════════════════╗\n";
    std::cout << "║       HFT FRAMEWORK UNIT TESTS       ║\n";
    std::cout << "╚══════════════════════════════════════╝\n\n";

    TEST(buy_fill_on_ask_cross);
    TEST(sell_fill_on_bid_cross);
    TEST(cancel_removes_order);
    TEST(pnl_tracker_cash_accounting);
    TEST(csv_reader_parse);
    TEST(market_order_fills_immediately);

    std::cout << "\n──────────────────────────────────────\n";
    std::cout << "  Results: " << tests_passed << "/" << tests_run << " passed\n";
    if (tests_passed < tests_run)
        std::cout << "  *** FAILURES DETECTED ***\n";
    else
        std::cout << "  All tests PASSED ✓\n";
    std::cout << "──────────────────────────────────────\n\n";

    return tests_passed == tests_run ? 0 : 1;
}
