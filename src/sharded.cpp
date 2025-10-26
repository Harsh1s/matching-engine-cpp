#include "sharded.hpp"

#include <cstdint>
#include <stdexcept>

namespace me {

ShardedMatchingEngine::ShardedMatchingEngine(int shard_count, const std::string& wal_root,
                                             AckPolicy ack_policy, const RiskLimits& risk_limits)
    : shard_count_(shard_count), risk_(risk_limits) {
    if (shard_count <= 0) throw std::invalid_argument("shard_count must be positive");
    engines_.resize(static_cast<std::size_t>(shard_count));
    wals_.reserve(static_cast<std::size_t>(shard_count));
    for (int i = 0; i < shard_count; ++i) {
        wals_.emplace_back(wal_root, i, ack_policy);
    }
    last_ingress_sequence_.assign(static_cast<std::size_t>(shard_count), 0);
}

int ShardedMatchingEngine::shard(const std::string& symbol) const {
    // 64-bit FNV-1a.
    std::uint64_t hash = 1469598103934665603ULL;
    for (char c : symbol) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return static_cast<int>(hash % static_cast<std::uint64_t>(shard_count_));
}

Json ShardedMatchingEngine::serialize_add(const AddOrder& cmd) const {
    Json j = Json::make_object();
    j.set("symbol", Json::from_string(cmd.symbol));
    j.set("order_id", Json::from_int(cmd.order_id));
    j.set("participant_id", Json::from_string(cmd.participant_id));
    j.set("side", Json::from_string(to_string(cmd.side)));
    j.set("price_ticks", Json::from_int(cmd.price_ticks));
    j.set("quantity", Json::from_int(cmd.quantity));
    j.set("time_in_force", Json::from_string(to_string(cmd.time_in_force)));
    j.set("stp", Json::from_string(to_string(cmd.stp)));
    return j;
}

Json ShardedMatchingEngine::serialize_replace(const ReplaceOrder& cmd) const {
    Json j = Json::make_object();
    j.set("symbol", Json::from_string(cmd.symbol));
    j.set("order_id", Json::from_int(cmd.order_id));
    j.set("new_order_id", Json::from_int(cmd.new_order_id));
    j.set("new_price_ticks", Json::from_int(cmd.new_price_ticks));
    j.set("new_quantity", Json::from_int(cmd.new_quantity));
    j.set("time_in_force", Json::from_string(to_string(cmd.time_in_force)));
    return j;
}

std::optional<ExecEvent> ShardedMatchingEngine::validate_ingress(int shard_id,
                                                                 std::optional<long long> ingress_sequence,
                                                                 const std::string& symbol,
                                                                 long long order_id) {
    if (!ingress_sequence.has_value()) return std::nullopt;
    if (*ingress_sequence <= last_ingress_sequence_[static_cast<std::size_t>(shard_id)]) {
        return engines_[static_cast<std::size_t>(shard_id)].reject(symbol, order_id, "OUT_OF_ORDER_INGRESS");
    }
    last_ingress_sequence_[static_cast<std::size_t>(shard_id)] = *ingress_sequence;
    return std::nullopt;
}

std::pair<std::vector<ExecEvent>, std::vector<Trade>> ShardedMatchingEngine::add(
    const AddOrder& cmd, std::optional<long long> ingress_sequence) {
    int s = shard(cmd.symbol);
    auto rejected = validate_ingress(s, ingress_sequence, cmd.symbol, cmd.order_id);
    if (rejected.has_value()) return {{*rejected}, {}};

    auto reason = risk_.check_add(cmd);
    if (reason.has_value()) {
        ExecEvent ev = engines_[static_cast<std::size_t>(s)].reject(cmd.symbol, cmd.order_id, *reason);
        Json record = Json::make_object();
        record.set("type", Json::from_string("risk_reject"));
        record.set("command", serialize_add(cmd));
        record.set("reason", Json::from_string(*reason));
        wals_[static_cast<std::size_t>(s)].append(record);
        return {{ev}, {}};
    }

    Json record = Json::make_object();
    record.set("type", Json::from_string("add"));
    record.set("command", serialize_add(cmd));
    wals_[static_cast<std::size_t>(s)].append(record);
    return engines_[static_cast<std::size_t>(s)].add(cmd);
}

std::vector<ExecEvent> ShardedMatchingEngine::cancel(const CancelOrder& cmd,
                                                     std::optional<long long> ingress_sequence) {
    int s = shard(cmd.symbol);
    auto rejected = validate_ingress(s, ingress_sequence, cmd.symbol, cmd.order_id);
    if (rejected.has_value()) return {*rejected};

    Json command = Json::make_object();
    command.set("symbol", Json::from_string(cmd.symbol));
    command.set("order_id", Json::from_int(cmd.order_id));
    Json record = Json::make_object();
    record.set("type", Json::from_string("cancel"));
    record.set("command", std::move(command));
    wals_[static_cast<std::size_t>(s)].append(record);
    return engines_[static_cast<std::size_t>(s)].cancel(cmd);
}

std::pair<std::vector<ExecEvent>, std::vector<Trade>> ShardedMatchingEngine::replace(
    const ReplaceOrder& cmd, std::optional<long long> ingress_sequence) {
    int s = shard(cmd.symbol);
    auto rejected = validate_ingress(s, ingress_sequence, cmd.symbol, cmd.order_id);
    if (rejected.has_value()) return {{*rejected}, {}};

    Json record = Json::make_object();
    record.set("type", Json::from_string("replace"));
    record.set("command", serialize_replace(cmd));
    wals_[static_cast<std::size_t>(s)].append(record);
    return engines_[static_cast<std::size_t>(s)].replace(cmd);
}

void ShardedMatchingEngine::snapshot_all() {
    for (std::size_t i = 0; i < engines_.size(); ++i) {
        wals_[i].save_snapshot(engines_[i].snapshot());
    }
}

void ShardedMatchingEngine::restore_from_snapshots() {
    for (std::size_t i = 0; i < engines_.size(); ++i) {
        auto snap = wals_[i].load_snapshot();
        if (snap.has_value()) engines_[i].load_snapshot(*snap);
    }
}

void ShardedMatchingEngine::apply_record(MatchingEngine& engine, const Json& record) {
    const Json* type = record.find("type");
    const Json* command = record.find("command");
    if (type == nullptr || command == nullptr || !command->is_object()) {
        throw MatchingEngineError("invalid wal record");
    }
    std::string cmd_type = type->as_string();
    auto get_str = [&](const char* key) {
        const Json* v = command->find(key);
        return v ? v->as_string() : std::string();
    };
    auto get_int = [&](const char* key) {
        const Json* v = command->find(key);
        return v ? v->as_int() : 0LL;
    };

    if (cmd_type == "add") {
        AddOrder cmd;
        cmd.symbol = get_str("symbol");
        cmd.order_id = get_int("order_id");
        cmd.participant_id = get_str("participant_id");
        cmd.side = side_from_string(get_str("side"));
        cmd.price_ticks = get_int("price_ticks");
        cmd.quantity = get_int("quantity");
        cmd.time_in_force = tif_from_string(get_str("time_in_force"));
        const Json* stp = command->find("stp");
        cmd.stp = stp ? stp_from_string(stp->as_string()) : SelfTradePrevention::None;
        engine.add(cmd);
    } else if (cmd_type == "cancel") {
        engine.cancel(CancelOrder{get_str("symbol"), get_int("order_id")});
    } else if (cmd_type == "replace") {
        ReplaceOrder cmd;
        cmd.symbol = get_str("symbol");
        cmd.order_id = get_int("order_id");
        cmd.new_order_id = get_int("new_order_id");
        cmd.new_price_ticks = get_int("new_price_ticks");
        cmd.new_quantity = get_int("new_quantity");
        cmd.time_in_force = tif_from_string(get_str("time_in_force"));
        engine.replace(cmd);
    }
    // "risk_reject" records are intentionally not replayed.
}

MatchingEngine ShardedMatchingEngine::replay_shard(int shard_id) {
    if (shard_id < 0 || shard_id >= shard_count_) throw std::out_of_range("invalid shard id");
    MatchingEngine rebuilt;
    auto snap = wals_[static_cast<std::size_t>(shard_id)].load_snapshot();
    if (snap.has_value()) rebuilt.load_snapshot(*snap);
    for (const Json& record : wals_[static_cast<std::size_t>(shard_id)].replay_commands()) {
        apply_record(rebuilt, record);
    }
    return rebuilt;
}

}  // namespace me
