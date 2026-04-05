#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "matching_engine.hpp"
#include "protocol.hpp"
#include "recovery.hpp"
#include "sharded.hpp"
#include "spsc_queue.hpp"
#include "threaded_engine.hpp"
#include "types.hpp"

using namespace me;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& name) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << name << "\n";
    }
}

void test_price_time_priority_and_passive_price() {
    MatchingEngine engine;
    engine.add(AddOrder{"NFLX", 1, "A", Side::Sell, 100, 5});
    engine.add(AddOrder{"NFLX", 2, "B", Side::Sell, 100, 5});
    auto [events, trades] = engine.add(AddOrder{"NFLX", 3, "C", Side::Buy, 101, 7});
    check(trades.size() == 2, "ptp: two trades");
    check(trades[0].seller_order_id == 1 && trades[1].seller_order_id == 2, "ptp: FIFO seller order");
    check(trades[0].quantity == 5 && trades[1].quantity == 2, "ptp: fill quantities");
    check(trades[0].price_ticks == 100 && trades[1].price_ticks == 100, "ptp: passive price");
    check(events.back().type == EventType::Executed, "ptp: last event executed");
}

void test_ioc_unfilled_remainder_is_cancelled() {
    MatchingEngine engine;
    engine.add(AddOrder{"GM", 1, "S", Side::Sell, 100, 3});
    auto [events, trades] = engine.add(AddOrder{"GM", 2, "B", Side::Buy, 100, 10, TimeInForce::IOC});
    long long filled = 0;
    for (const auto& t : trades) filled += t.quantity;
    check(filled == 3, "ioc: filled 3");
    check(events.back().type == EventType::Cancelled, "ioc: last event cancelled");
    check(payload_int(events.back().payload, "remaining") == 7, "ioc: remaining 7");
}

void test_fok_reject_when_not_fully_fillable() {
    MatchingEngine engine;
    engine.add(AddOrder{"K", 1, "S", Side::Sell, 50, 4});
    auto [events, trades] = engine.add(AddOrder{"K", 2, "B", Side::Buy, 50, 5, TimeInForce::FOK});
    check(trades.empty(), "fok: no trades");
    check(events.front().type == EventType::Rejected, "fok: rejected");
    long long bid = 0;
    check(!engine.best_bid("K", bid), "fok: no resting bid");
}

void test_cancel_and_replace() {
    MatchingEngine engine;
    engine.add(AddOrder{"GM", 10, "A", Side::Buy, 99, 5});
    auto cancel_events = engine.cancel(CancelOrder{"GM", 10});
    check(cancel_events.front().type == EventType::Cancelled, "cancel: cancelled");
    engine.add(AddOrder{"GM", 11, "A", Side::Buy, 99, 5});
    auto [replace_events, replace_trades] = engine.replace(ReplaceOrder{"GM", 11, 12, 98, 6});
    check(replace_events[0].type == EventType::Cancelled, "replace: first cancelled");
    check(replace_events[1].type == EventType::Accepted, "replace: then accepted");
    long long bid = 0;
    check(engine.best_bid("GM", bid) && bid == 98, "replace: new best bid 98");
}

void test_stp_cancel_both() {
    MatchingEngine engine;
    engine.add(AddOrder{"NFLX", 1, "A", Side::Sell, 100, 5});
    auto [events, trades] = engine.add(
        AddOrder{"NFLX", 2, "A", Side::Buy, 100, 3, TimeInForce::GTC, SelfTradePrevention::CancelBoth});
    check(trades.empty(), "stp: no trades");
    std::vector<long long> cancelled;
    for (const auto& ev : events) {
        if (ev.type == EventType::Cancelled) cancelled.push_back(payload_int(ev.payload, "order_id"));
    }
    check(cancelled.size() == 2 && cancelled[0] == 1 && cancelled[1] == 2, "stp: cancels passive then aggressor");
}

void test_no_crossed_book_under_random_flow() {
    MatchingEngine engine;
    std::vector<long long> live;
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<int> price_dist(95, 105);
    std::uniform_int_distribution<int> qty_dist(1, 10);
    long long next_id = 1;
    bool ok = true;
    for (int i = 0; i < 500; ++i) {
        if (!live.empty() && unit(rng) < 0.25) {
            std::uniform_int_distribution<size_t> pick(0, live.size() - 1);
            size_t idx = pick(rng);
            long long oid = live[idx];
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
            engine.cancel(CancelOrder{"NFLX", oid});
        } else {
            Side side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
            long long price = price_dist(rng);
            long long qty = qty_dist(rng);
            TimeInForce tif = unit(rng) < 0.8 ? TimeInForce::GTC : TimeInForce::IOC;
            auto [events, trades] = engine.add(AddOrder{"NFLX", next_id, "P", side, price, qty, tif});
            for (const auto& ev : events) {
                if (ev.type == EventType::Rested && payload_int(ev.payload, "order_id") == next_id) {
                    live.push_back(next_id);
                }
            }
            ++next_id;
        }
        long long bid = 0, ask = 0;
        bool hb = engine.best_bid("NFLX", bid);
        bool ha = engine.best_ask("NFLX", ask);
        if (hb && ha && bid >= ask) ok = false;
    }
    check(ok, "random flow: book never crossed");
}

void test_volume_conservation() {
    MatchingEngine engine;
    long long accepted = 0, executed = 0, cancelled = 0, max_seq = 0;
    bool monotonic = true;
    std::vector<AddOrder> commands = {
        {"GM", 1, "A", Side::Buy, 100, 8},
        {"GM", 2, "B", Side::Sell, 101, 4},
        {"GM", 3, "C", Side::Sell, 100, 3},
        {"GM", 4, "D", Side::Buy, 101, 2, TimeInForce::IOC},
    };
    for (const auto& cmd : commands) {
        auto [events, trades] = engine.add(cmd);
        for (const auto& ev : events) {
            if (ev.sequence <= max_seq) monotonic = false;
            max_seq = ev.sequence;
            if (ev.type == EventType::Accepted) accepted += payload_int(ev.payload, "qty");
            if (ev.type == EventType::Executed) executed += payload_int(ev.payload, "qty");
            if (ev.type == EventType::Cancelled) cancelled += payload_int(ev.payload, "remaining");
        }
    }
    check(monotonic, "conservation: sequences monotonic");

    Json snap = engine.snapshot();
    long long resting = 0;
    const Json* books = snap.find("books");
    for (const auto& book_kv : books->object_value) {
        for (const char* side : {"bids", "asks"}) {
            const Json* levels = book_kv.second.find(side);
            for (const auto& level : levels->array_value) {
                const Json* orders = level.find("orders");
                for (const auto& order : orders->array_value) {
                    resting += order.find("remaining")->as_int();
                }
            }
        }
    }
    check(accepted - 2 * executed - cancelled == resting, "conservation: volume balances");
}

void test_snapshot_round_trip() {
    MatchingEngine engine;
    engine.add(AddOrder{"AAPL", 1, "A", Side::Buy, 100, 5});
    engine.add(AddOrder{"AAPL", 2, "B", Side::Sell, 105, 4});
    Json snap = engine.snapshot();
    std::string dumped = snap.dump();
    Json reparsed = Json::parse(dumped);
    MatchingEngine restored;
    restored.load_snapshot(reparsed);
    long long bid = 0, ask = 0;
    check(restored.best_bid("AAPL", bid) && bid == 100, "snapshot: restored bid");
    check(restored.best_ask("AAPL", ask) && ask == 105, "snapshot: restored ask");
}

void test_protocol_round_trip() {
    AddOrder original{"AAPL", 42, "DESK1", Side::Buy, 1500, 250, TimeInForce::FOK,
                      SelfTradePrevention::CancelAggressor};
    AddOrder decoded = unpack_add_order(pack_add_order(original));
    check(decoded.symbol == "AAPL" && decoded.participant_id == "DESK1", "protocol: strings");
    check(decoded.order_id == 42 && decoded.price_ticks == 1500 && decoded.quantity == 250, "protocol: integers");
    check(decoded.side == Side::Buy && decoded.time_in_force == TimeInForce::FOK &&
              decoded.stp == SelfTradePrevention::CancelAggressor,
          "protocol: enums");
}

void test_sharded_wal_replay() {
    std::string root = "test_data_replay";
    int target_shard = 0;
    long long expected_bid = 0;
    {
        ShardedMatchingEngine sharded(4, root);
        sharded.add(AddOrder{"AAPL", 1, "A", Side::Buy, 100, 5});
        sharded.add(AddOrder{"AAPL", 2, "B", Side::Buy, 99, 3});
        sharded.add(AddOrder{"AAPL", 3, "C", Side::Sell, 100, 2});
        target_shard = sharded.shard_for_symbol("AAPL");
        sharded.engine_for_symbol("AAPL").best_bid("AAPL", expected_bid);
    }
    {
        ShardedMatchingEngine sharded(4, root);
        MatchingEngine rebuilt = sharded.replay_shard(target_shard);
        long long bid = 0;
        check(rebuilt.best_bid("AAPL", bid) && bid == expected_bid, "sharded: WAL replay reconstructs book");
    }
}

void test_spsc_queue() {
    SpscQueue<int> q(4);  // rounds to capacity 4
    check(q.empty(), "spsc: starts empty");
    for (int i = 0; i < 4; ++i) check(q.push(i), "spsc: push within capacity");
    check(!q.push(99), "spsc: push rejected when full");
    int v = -1;
    check(q.pop(v) && v == 0, "spsc: FIFO pop");
    check(q.push(4), "spsc: push after pop");
    int sum = v;
    while (q.pop(v)) sum += v;
    check(sum == (1 + 2 + 3 + 4), "spsc: drained all in order");
}

void test_threaded_engine() {
    ThreadedShardedEngine engine(2, "test_data_threaded");
    engine.start();
    engine.submit(AddOrder{"AAPL", 1, "A", Side::Sell, 100, 5});
    engine.submit(AddOrder{"AAPL", 2, "B", Side::Sell, 100, 5});
    engine.submit(AddOrder{"AAPL", 3, "C", Side::Buy, 101, 7});

    std::vector<ResultMsg> all;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (all.size() < 3 && std::chrono::steady_clock::now() < deadline) {
        for (auto& r : engine.drain()) all.push_back(std::move(r));
        if (all.size() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine.stop();

    check(all.size() == 3, "threaded: three results delivered");
    long long filled = 0;
    for (const auto& r : all) {
        for (const auto& t : r.trades) filled += t.quantity;
    }
    check(filled == 7, "threaded: aggressor filled 7 across workers");
}

void test_codec_cancel_replace() {
    CancelOrder cancel{"AAPL", 7};
    check(unpack_cancel_order(pack_cancel_order(cancel)).order_id == 7, "codec: cancel round trip");
    ReplaceOrder rep{"MSFT", 1, 2, 200, 9, TimeInForce::IOC};
    ReplaceOrder dec = unpack_replace_order(pack_replace_order(rep));
    check(dec.new_order_id == 2 && dec.new_price_ticks == 200 && dec.new_quantity == 9 &&
              dec.symbol == "MSFT" && dec.time_in_force == TimeInForce::IOC,
          "codec: replace round trip");
}

void test_wal_crc_recovery() {
    std::string root = "test_data_wal";
    { WalStore w(root, 0); w.save_snapshot(Json::make_object()); }  // truncate to a clean WAL
    Json rec = Json::make_object();
    rec.set("type", Json::from_string("add"));
    { WalStore w(root, 0); w.append(rec); }  // dtor flushes
    {
        WalStore w(root, 0);
        auto cmds = w.replay_commands();
        check(cmds.size() == 1 && cmds[0].find("type")->as_string() == "add", "wal: record survives reopen");
    }
    {
        std::ofstream out(root + "/shard_0.wal.jsonl", std::ios::binary | std::ios::app);
        out << "deadbeef {\"type\":\"add\"}\n";  // deliberately wrong CRC (torn tail)
    }
    {
        WalStore w(root, 0);
        check(w.replay_commands().size() == 1, "wal: corrupt tail discarded via crc");
    }
}

}  // namespace

int main() {
    test_price_time_priority_and_passive_price();
    test_ioc_unfilled_remainder_is_cancelled();
    test_fok_reject_when_not_fully_fillable();
    test_cancel_and_replace();
    test_stp_cancel_both();
    test_no_crossed_book_under_random_flow();
    test_volume_conservation();
    test_snapshot_round_trip();
    test_protocol_round_trip();
    test_sharded_wal_replay();
    test_spsc_queue();
    test_threaded_engine();
    test_codec_cancel_replace();
    test_wal_crc_recovery();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed.\n";
    return 0;
}
