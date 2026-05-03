#include "matching_engine.hpp"

namespace me {

MatchingEngine::Book& MatchingEngine::book_for(const std::string& symbol) {
    return books_[symbol];
}

const MatchingEngine::Book* MatchingEngine::find_book(const std::string& symbol) const {
    auto it = books_.find(symbol);
    return it == books_.end() ? nullptr : &it->second;
}

ExecEvent MatchingEngine::event(EventType type, const std::string& symbol, Payload payload) {
    ++sequence_;
    return ExecEvent{sequence_, type, symbol, std::move(payload)};
}

ExecEvent MatchingEngine::reject(const std::string& symbol, long long order_id, const std::string& reason) {
    return event(EventType::Rejected, symbol,
                 {{"order_id", order_id}, {"reason", reason}});
}

long long MatchingEngine::available_volume(const Book& book, Side side, long long limit_price) const {
    long long total = 0;
    if (side == Side::Sell) {
        for (const auto& kv : book.asks) {
            if (kv.first > limit_price) break;
            for (const auto& order : kv.second) total += order.remaining;
        }
    } else {
        for (auto it = book.bids.rbegin(); it != book.bids.rend(); ++it) {
            if (it->first < limit_price) break;
            for (const auto& order : it->second) total += order.remaining;
        }
    }
    return total;
}

void MatchingEngine::rest_order(Book& book, const AddOrder& cmd, long long remaining) {
    auto& side_map = (cmd.side == Side::Buy) ? book.bids : book.asks;
    auto level_it = side_map.find(cmd.price_ticks);
    if (level_it == side_map.end()) {
level_it = side_map.emplace(cmd.price_ticks, Level{}).first;
    }
    Level& level = level_it->second;
    RestingOrder order{cmd.symbol, cmd.order_id, cmd.participant_id, cmd.side,
                       cmd.price_ticks, remaining, sequence_};
    auto iter = level.insert(level.end(), std::move(order));
    index_[cmd.order_id] = Location{cmd.symbol, cmd.side, cmd.price_ticks, iter};
}

void MatchingEngine::remove_resting(long long order_id) {
    auto found = index_.find(order_id);
    if (found == index_.end()) return;
    Location loc = found->second;
    Book& book = books_[loc.symbol];
    auto& side_map = (loc.side == Side::Buy) ? book.bids : book.asks;
    auto level_it = side_map.find(loc.price);
    if (level_it != side_map.end()) {
        level_it->second.erase(loc.iter);
        if (level_it->second.empty()) side_map.erase(level_it);
    }
    index_.erase(found);
}

Trade MatchingEngine::build_trade(const AddOrder& active, const RestingOrder& passive, long long qty) const {
    if (qty <= 0) throw MatchingEngineError("non-positive trade quantity");
    if (active.symbol != passive.symbol) throw MatchingEngineError("symbol mismatch");
    Trade trade;
    trade.symbol = active.symbol;
    trade.price_ticks = passive.price_ticks;
    trade.quantity = qty;
    trade.passive_order_id = passive.order_id;
    trade.aggressive_order_id = active.order_id;
    if (active.side == Side::Buy) {
        trade.buyer_order_id = active.order_id;
        trade.buyer_participant_id = active.participant_id;
        trade.seller_order_id = passive.order_id;
        trade.seller_participant_id = passive.participant_id;
    } else {
        trade.buyer_order_id = passive.order_id;
        trade.buyer_participant_id = passive.participant_id;
        trade.seller_order_id = active.order_id;
        trade.seller_participant_id = active.participant_id;
    }
    return trade;
}

void MatchingEngine::check_not_crossed(const std::string& symbol, const Book& book) const {
    if (book.bids.empty() || book.asks.empty()) return;
    long long bb = std::prev(book.bids.end())->first;
    long long ba = book.asks.begin()->first;
    if (bb >= ba) {
        throw MatchingEngineError("crossed book on " + symbol);
    }
}

std::pair<std::vector<ExecEvent>, std::vector<Trade>> MatchingEngine::add(const AddOrder& cmd) {
    Book& book = book_for(cmd.symbol);

    if (cmd.time_in_force == TimeInForce::FOK &&
        available_volume(book, opposite(cmd.side), cmd.price_ticks) < cmd.quantity) {
        std::vector<ExecEvent> events{
            event(EventType::Rejected, cmd.symbol,
                  {{"order_id", cmd.order_id}, {"reason", std::string("FOK_UNFILLABLE")}})};
        return {std::move(events), {}};
    }

    std::vector<ExecEvent> events;
    std::vector<Trade> trades;
    events.reserve(8);
    trades.reserve(8);
    events.push_back(event(EventType::Accepted, cmd.symbol,
                           {{"order_id", cmd.order_id}, {"qty", cmd.quantity}}));

    long long remaining = cmd.quantity;
    auto& opposite_map = (cmd.side == Side::Buy) ? book.asks : book.bids;

    while (remaining > 0 && !opposite_map.empty()) {
        auto level_it = (cmd.side == Side::Buy) ? opposite_map.begin() : std::prev(opposite_map.end());
        long long best_price = level_it->first;
        if (cmd.time_in_force != TimeInForce::MARKET) {
            if (cmd.side == Side::Buy && cmd.price_ticks < best_price) break;
            if (cmd.side == Side::Sell && cmd.price_ticks > best_price) break;
        }

        Level& level = level_it->second;
        while (remaining > 0 && !level.empty()) {
            RestingOrder& passive = level.front();
            long long passive_id = passive.order_id;

            if (passive.participant_id == cmd.participant_id && cmd.stp != SelfTradePrevention::None) {
                if (cmd.stp == SelfTradePrevention::CancelPassive || cmd.stp == SelfTradePrevention::CancelBoth) {
                    events.push_back(event(EventType::Cancelled, cmd.symbol,
                                           {{"order_id", passive.order_id},
                                            {"remaining", passive.remaining},
                                            {"reason", std::string("SELF_TRADE_PREVENTION")}}));
                    index_.erase(passive_id);
                    level.pop_front();
                }
                if (cmd.stp == SelfTradePrevention::CancelAggressor || cmd.stp == SelfTradePrevention::CancelBoth) {
                    events.push_back(event(EventType::Cancelled, cmd.symbol,
                                           {{"order_id", cmd.order_id},
                                            {"remaining", remaining},
                                            {"reason", std::string("SELF_TRADE_PREVENTION")}}));
                    remaining = 0;
                    break;
                }
                continue;
            }

            long long passive_remaining = passive.remaining;
            long long fill = remaining < passive_remaining ? remaining : passive_remaining;
            if (fill <= 0) throw MatchingEngineError("non-positive fill quantity");

            remaining -= fill;
            long long new_passive_remaining = passive_remaining - fill;
            passive.remaining = new_passive_remaining;
            if (new_passive_remaining < 0) throw MatchingEngineError("passive overfill");

            trades.push_back(build_trade(cmd, passive, fill));
            events.push_back(event(EventType::Executed, cmd.symbol,
                                   {{"aggressive_order_id", cmd.order_id},
                                    {"passive_order_id", passive_id},
                                    {"price_ticks", passive.price_ticks},
                                    {"qty", fill}}));

            if (new_passive_remaining == 0) {
                index_.erase(passive_id);
                level.pop_front();
            }
        }

        if (level.empty()) {
            opposite_map.erase(level_it);
        }
    }

    if (remaining > 0) {
        if (cmd.time_in_force == TimeInForce::GTC) {
            rest_order(book, cmd, remaining);
            events.push_back(event(EventType::Rested, cmd.symbol,
                                   {{"order_id", cmd.order_id}, {"remaining", remaining}}));
            check_not_crossed(cmd.symbol, book);
        } else if (cmd.time_in_force == TimeInForce::IOC || cmd.time_in_force == TimeInForce::MARKET) {
            events.push_back(event(EventType::Cancelled, cmd.symbol,
                                   {{"order_id", cmd.order_id}, {"remaining", remaining}}));
        }
    }

    return {std::move(events), std::move(trades)};
}

std::vector<ExecEvent> MatchingEngine::cancel(const CancelOrder& cmd) {
    auto found = index_.find(cmd.order_id);
    if (found == index_.end()) {
        return {event(EventType::Rejected, cmd.symbol,
                      {{"order_id", cmd.order_id}, {"reason", std::string("UNKNOWN_ORDER")}})};
    }
    long long remaining = found->second.iter->remaining;
    remove_resting(cmd.order_id);
    return {event(EventType::Cancelled, cmd.symbol,
                  {{"order_id", cmd.order_id}, {"remaining", remaining}})};
}

std::pair<std::vector<ExecEvent>, std::vector<Trade>> MatchingEngine::replace(const ReplaceOrder& cmd) {
    auto found = index_.find(cmd.order_id);
    if (found == index_.end()) {
        std::vector<ExecEvent> events{event(EventType::Rejected, cmd.symbol,
                                            {{"order_id", cmd.order_id}, {"reason", std::string("UNKNOWN_ORDER")}})};
        return {std::move(events), {}};
    }
    std::string participant = found->second.iter->participant_id;
    Side side = found->second.side;

    std::vector<ExecEvent> events = cancel(CancelOrder{cmd.symbol, cmd.order_id});

    AddOrder add_cmd;
    add_cmd.symbol = cmd.symbol;
    add_cmd.order_id = cmd.new_order_id;
    add_cmd.participant_id = participant;
    add_cmd.side = side;
    add_cmd.price_ticks = cmd.new_price_ticks;
    add_cmd.quantity = cmd.new_quantity;
    add_cmd.time_in_force = cmd.time_in_force;
    add_cmd.stp = SelfTradePrevention::None;

    auto add_result = add(add_cmd);
    events.insert(events.end(), add_result.first.begin(), add_result.first.end());
    return {std::move(events), std::move(add_result.second)};
}

bool MatchingEngine::best_bid(const std::string& symbol, long long& out) const {
    const Book* book = find_book(symbol);
    if (book == nullptr || book->bids.empty()) return false;
    out = std::prev(book->bids.end())->first;
    return true;
}

bool MatchingEngine::best_ask(const std::string& symbol, long long& out) const {
    const Book* book = find_book(symbol);
    if (book == nullptr || book->asks.empty()) return false;
    out = book->asks.begin()->first;
    return true;
}

Json MatchingEngine::serialize_side(const Book& book, Side side) const {
    Json levels = Json::make_array();
    if (side == Side::Buy) {
        for (auto it = book.bids.rbegin(); it != book.bids.rend(); ++it) {
            Json level = Json::make_object();
            level.set("price_ticks", Json::from_int(it->first));
            Json orders = Json::make_array();
            for (const auto& o : it->second) {
                Json order = Json::make_object();
                order.set("order_id", Json::from_int(o.order_id));
                order.set("participant_id", Json::from_string(o.participant_id));
                order.set("remaining", Json::from_int(o.remaining));
                orders.push_back(std::move(order));
            }
            level.set("orders", std::move(orders));
            levels.push_back(std::move(level));
        }
    } else {
        for (const auto& kv : book.asks) {
            Json level = Json::make_object();
            level.set("price_ticks", Json::from_int(kv.first));
            Json orders = Json::make_array();
            for (const auto& o : kv.second) {
                Json order = Json::make_object();
                order.set("order_id", Json::from_int(o.order_id));
                order.set("participant_id", Json::from_string(o.participant_id));
                order.set("remaining", Json::from_int(o.remaining));
                orders.push_back(std::move(order));
            }
            level.set("orders", std::move(orders));
            levels.push_back(std::move(level));
        }
    }
    return levels;
}

Json MatchingEngine::snapshot() const {
    Json root = Json::make_object();
    root.set("sequence", Json::from_int(sequence_));
    Json books = Json::make_object();
    for (const auto& kv : books_) {
        Json book = Json::make_object();
        book.set("bids", serialize_side(kv.second, Side::Buy));
        book.set("asks", serialize_side(kv.second, Side::Sell));
        books.set(kv.first, std::move(book));
    }
    root.set("books", std::move(books));
    return root;
}

void MatchingEngine::restore_side(const std::string& symbol, const Json& levels, Side side) {
    if (!levels.is_array()) return;
    Book& book = book_for(symbol);
    for (const auto& level : levels.array_value) {
        const Json* price = level.find("price_ticks");
        if (price == nullptr) continue;
        const Json* orders = level.find("orders");
        if (orders == nullptr || !orders->is_array()) continue;
        for (const auto& o : orders->array_value) {
            const Json* order_id = o.find("order_id");
            const Json* participant = o.find("participant_id");
            const Json* remaining = o.find("remaining");
            if (order_id == nullptr || participant == nullptr || remaining == nullptr) continue;
            AddOrder cmd;
            cmd.symbol = symbol;
            cmd.order_id = order_id->as_int();
            cmd.participant_id = participant->as_string();
            cmd.side = side;
            cmd.price_ticks = price->as_int();
            cmd.quantity = remaining->as_int();
            cmd.time_in_force = TimeInForce::GTC;
            rest_order(book, cmd, remaining->as_int());
        }
    }
}

void MatchingEngine::load_snapshot(const Json& snap) {
    books_.clear();
    index_.clear();
    const Json* sequence = snap.find("sequence");
    sequence_ = sequence ? sequence->as_int() : 0;
    const Json* books = snap.find("books");
    if (books == nullptr || !books->is_object()) {
        throw MatchingEngineError("invalid snapshot: 'books' must be an object");
    }
    for (const auto& kv : books->object_value) {
        const Json& book_data = kv.second;
        if (!book_data.is_object()) {
            throw MatchingEngineError("invalid snapshot: book data must be an object");
        }
        const Json* bids = book_data.find("bids");
        const Json* asks = book_data.find("asks");
        if (bids) restore_side(kv.first, *bids, Side::Buy);
        if (asks) restore_side(kv.first, *asks, Side::Sell);
    }
}

}  // namespace me
