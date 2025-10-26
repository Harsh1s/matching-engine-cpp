#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "matching_engine.hpp"
#include "recovery.hpp"
#include "risk.hpp"
#include "types.hpp"

namespace me {

// Single-process symbol-sharded engine. Each shard owns one MatchingEngine
// and one WAL. Commands route to a shard by a stable hash of the symbol.
//
// Note: the Python original sharded on blake2b(symbol); this standalone C++
// port uses a 64-bit FNV-1a hash. Sharding only needs to be deterministic and
// balanced for a self-contained store, so WAL files are not cross-compatible
// with the Python build by design.
class ShardedMatchingEngine {
public:
    ShardedMatchingEngine(int shard_count,
                          const std::string& wal_root = "data",
                          AckPolicy ack_policy = AckPolicy::Async,
                          const RiskLimits& risk_limits = RiskLimits{});

    std::pair<std::vector<ExecEvent>, std::vector<Trade>> add(
        const AddOrder& cmd, std::optional<long long> ingress_sequence = std::nullopt);
    std::vector<ExecEvent> cancel(
        const CancelOrder& cmd, std::optional<long long> ingress_sequence = std::nullopt);
    std::pair<std::vector<ExecEvent>, std::vector<Trade>> replace(
        const ReplaceOrder& cmd, std::optional<long long> ingress_sequence = std::nullopt);

    void snapshot_all();
    void restore_from_snapshots();
    MatchingEngine replay_shard(int shard_id);

    int shard_for_symbol(const std::string& symbol) const { return shard(symbol); }
    MatchingEngine& engine_for_symbol(const std::string& symbol) { return engines_[shard(symbol)]; }

private:
    int shard_count_;
    std::vector<MatchingEngine> engines_;
    std::vector<WalStore> wals_;
    PreTradeRisk risk_;
    std::vector<long long> last_ingress_sequence_;

    int shard(const std::string& symbol) const;
    Json serialize_add(const AddOrder& cmd) const;
    Json serialize_replace(const ReplaceOrder& cmd) const;
    void apply_record(MatchingEngine& engine, const Json& record);
    std::optional<ExecEvent> validate_ingress(int shard_id, std::optional<long long> ingress_sequence,
                                               const std::string& symbol, long long order_id);
};

}  // namespace me
