#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "matching_engine.hpp"
#include "protocol.hpp"
#include "sharded.hpp"
#include "threaded_engine.hpp"
#include "types.hpp"

using namespace me;

namespace {

void print_events(const std::vector<ExecEvent>& events) {
    for (const auto& ev : events) {
        std::cout << "  seq=" << ev.sequence << " " << to_string(ev.type) << " " << ev.symbol;
        for (const auto& kv : ev.payload) {
            std::cout << " " << kv.first << "=";
            if (std::holds_alternative<long long>(kv.second)) {
                std::cout << std::get<long long>(kv.second);
            } else {
                std::cout << std::get<std::string>(kv.second);
            }
        }
        std::cout << "\n";
    }
}

void print_trades(const std::vector<Trade>& trades) {
    for (const auto& t : trades) {
        std::cout << "  TRADE " << t.symbol << " px=" << t.price_ticks << " qty=" << t.quantity
                  << " buyer=" << t.buyer_order_id << " seller=" << t.seller_order_id << "\n";
    }
}

}  // namespace

int main() {
    std::cout << "=== Price-time priority + passive-price execution ===\n";
    MatchingEngine engine;
    engine.add(AddOrder{"NFLX", 1, "A", Side::Sell, 100, 5});
    engine.add(AddOrder{"NFLX", 2, "B", Side::Sell, 100, 5});
    auto [events, trades] = engine.add(AddOrder{"NFLX", 3, "C", Side::Buy, 101, 7});
    print_events(events);
    print_trades(trades);

    std::cout << "\n=== IOC remainder is cancelled ===\n";
    MatchingEngine ioc;
    ioc.add(AddOrder{"GM", 1, "S", Side::Sell, 100, 3});
    auto ioc_res = ioc.add(AddOrder{"GM", 2, "B", Side::Buy, 100, 10, TimeInForce::IOC});
    print_events(ioc_res.first);
    print_trades(ioc_res.second);

    std::cout << "\n=== FOK rejected when not fully fillable ===\n";
    MatchingEngine fok;
    fok.add(AddOrder{"K", 1, "S", Side::Sell, 50, 4});
    auto fok_res = fok.add(AddOrder{"K", 2, "B", Side::Buy, 50, 5, TimeInForce::FOK});
    print_events(fok_res.first);

    std::cout << "\n=== Self-trade prevention (CANCEL_BOTH) ===\n";
    MatchingEngine stp;
    stp.add(AddOrder{"NFLX", 1, "A", Side::Sell, 100, 5});
    auto stp_res = stp.add(AddOrder{"NFLX", 2, "A", Side::Buy, 100, 3,
                                    TimeInForce::GTC, SelfTradePrevention::CancelBoth});
    print_events(stp_res.first);
    print_trades(stp_res.second);

    std::cout << "\n=== Binary add-order codec round trip ===\n";
    AddOrder original{"AAPL", 42, "DESK1", Side::Buy, 1500, 250, TimeInForce::IOC,
                      SelfTradePrevention::CancelPassive};
    AddOrder decoded = unpack_add_order(pack_add_order(original));
    std::cout << "  symbol=" << decoded.symbol << " order_id=" << decoded.order_id
              << " price=" << decoded.price_ticks << " qty=" << decoded.quantity
              << " tif=" << to_string(decoded.time_in_force) << " stp=" << to_string(decoded.stp) << "\n";

    std::cout << "\n=== Sharded engine with WAL replay ===\n";
    {
        ShardedMatchingEngine sharded(4, "demo_data");
        sharded.add(AddOrder{"AAPL", 1, "A", Side::Buy, 100, 5});
        sharded.add(AddOrder{"AAPL", 2, "B", Side::Sell, 100, 2});
        sharded.add(AddOrder{"MSFT", 3, "C", Side::Buy, 200, 4});
        int shard_id = sharded.shard_for_symbol("AAPL");
        MatchingEngine rebuilt = sharded.replay_shard(shard_id);
        long long bid = 0;
        bool has_bid = rebuilt.best_bid("AAPL", bid);
        std::cout << "  AAPL routed to shard " << shard_id
                  << "; replayed best_bid=" << (has_bid ? std::to_string(bid) : std::string("none")) << "\n";
    }

    std::cout << "\n=== Multi-core pipeline (SPSC queues + per-shard workers) ===\n";
    {
        ThreadedShardedEngine pipeline(4, "demo_data_threaded");
        pipeline.start();
        pipeline.submit(AddOrder{"AAPL", 1, "A", Side::Sell, 100, 5});
        pipeline.submit(AddOrder{"AAPL", 2, "B", Side::Sell, 100, 5});
        pipeline.submit(AddOrder{"AAPL", 3, "C", Side::Buy, 101, 7});
        pipeline.submit(Command{CancelOrder{"AAPL", 0}});  // unknown order -> reject

        std::vector<ResultMsg> results;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (results.size() < 4 && std::chrono::steady_clock::now() < deadline) {
            for (auto& r : pipeline.drain()) results.push_back(std::move(r));
            if (results.size() < 4) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        pipeline.stop();
        long long filled = 0;
        for (const auto& r : results) {
            for (const auto& t : r.trades) filled += t.quantity;
        }
        std::cout << "  drained " << results.size() << " async results; aggressor filled " << filled << "\n";
    }

    std::cout << "\nDone.\n";
    return 0;
}
