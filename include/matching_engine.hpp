#pragma once

#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "types.hpp"

namespace me {

class MatchingEngineError : public std::runtime_error {
public:
    explicit MatchingEngineError(const std::string& message) : std::runtime_error(message) {}
};

struct RestingOrder {
    std::string symbol;
    long long order_id = 0;
    std::string participant_id;
    Side side = Side::Buy;
    long long price_ticks = 0;
    long long remaining = 0;
    long long entered_sequence = 0;
};

// Single-writer price-time-priority matching engine for one or more symbols.
//
// Per-price-level FIFO is a std::list (insertion order == time priority).
// Price levels live in an ordered std::map keyed by tick price:
//   * best bid  = highest key  (std::prev(bids.end()))
//   * best ask  = lowest key   (asks.begin())
// A global index maps order_id -> its physical location for O(1) cancel.
class MatchingEngine {
public:
    using Level = std::list<RestingOrder>;
    struct Book {
        std::map<long long, Level> bids;
        std::map<long long, Level> asks;
    };

    MatchingEngine() = default;

    std::pair<std::vector<ExecEvent>, std::vector<Trade>> add(const AddOrder& cmd);
    std::vector<ExecEvent> cancel(const CancelOrder& cmd);
    std::pair<std::vector<ExecEvent>, std::vector<Trade>> replace(const ReplaceOrder& cmd);
    bool best_bid(const std::string& symbol, long long& out) const;
    bool best_ask(const std::string& symbol, long long& out) const;

private:
    struct Location {
        std::string symbol;
        Side side = Side::Buy;
        long long price = 0;
        Level::iterator iter;
    };

    std::unordered_map<std::string, Book> books_;
    long long sequence_ = 0;
    std::unordered_map<long long, Location> index_;

    Book& book_for(const std::string& symbol);
    const Book* find_book(const std::string& symbol) const;
    ExecEvent event(EventType type, const std::string& symbol, Payload payload);
    long long available_volume(const Book& book, Side side, long long limit_price) const;
    void rest_order(Book& book, const AddOrder& cmd, long long remaining);
    void remove_resting(long long order_id);
    Trade build_trade(const AddOrder& active, const RestingOrder& passive, long long qty) const;
    void check_not_crossed(const std::string& symbol, const Book& book) const;
};

}  // namespace me
