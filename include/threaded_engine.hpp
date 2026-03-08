#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "gateway.hpp"
#include "sharded.hpp"
#include "spsc_queue.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace me {

struct CommandMsg {
    Command command;
    std::optional<long long> ingress_sequence;
    std::uint64_t correlation = 0;
};

struct ResultMsg {
    std::uint64_t correlation = 0;
    std::vector<ExecEvent> events;
    std::vector<Trade> trades;
};

// Multi-core front end for ShardedMatchingEngine. One pinned worker thread per
// shard, each fed by its own lock-free SPSC inbound queue and publishing to its
// own SPSC outbound queue.
//
// Threading contract (standard staged-pipeline model):
//   * submit() is called from a single producer thread.
//   * drain()/for_each_result() is called from a single consumer thread.
// Each worker only ever touches the state of its own shard, so the underlying
// engine needs no locks.
class ThreadedShardedEngine {
public:
    ThreadedShardedEngine(int shard_count, const std::string& wal_root = "data",
                          AckPolicy ack_policy = AckPolicy::Async,
                          const RiskLimits& risk_limits = RiskLimits{},
                          std::size_t queue_capacity = 4096)
        : engine_(shard_count, wal_root, ack_policy, risk_limits), shard_count_(shard_count) {
        for (int i = 0; i < shard_count; ++i) {
            inbound_.push_back(std::make_unique<SpscQueue<CommandMsg>>(queue_capacity));
            outbound_.push_back(std::make_unique<SpscQueue<ResultMsg>>(queue_capacity));
        }
    }

    ~ThreadedShardedEngine() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        for (int i = 0; i < shard_count_; ++i) {
            workers_.emplace_back([this, i] { run_worker(i); });
            pin_to_core(workers_.back(), i);
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    // Enqueue a command; spins until the target shard queue has room. Returns
    // the correlation id echoed back on the matching ResultMsg.
    std::uint64_t submit(Command command, std::optional<long long> ingress_sequence = std::nullopt) {
        std::uint64_t id = ++next_correlation_;
        int s = shard_of(command);
        CommandMsg msg{std::move(command), ingress_sequence, id};
        while (!inbound_[static_cast<std::size_t>(s)]->push(std::move(msg))) {
            cpu_relax();
        }
        return id;
    }

    // Drains all currently available results across shards into a vector.
    std::vector<ResultMsg> drain() {
        std::vector<ResultMsg> out;
        ResultMsg msg;
        for (auto& q : outbound_) {
            while (q->pop(msg)) out.push_back(std::move(msg));
        }
        return out;
    }

    int shard_count() const { return shard_count_; }

private:
    int shard_of(const Command& command) const {
        return std::visit([this](const auto& c) { return engine_.shard_for_symbol(c.symbol); }, command);
    }

    void run_worker(int shard_id) {
        auto& in = *inbound_[static_cast<std::size_t>(shard_id)];
        auto& out = *outbound_[static_cast<std::size_t>(shard_id)];
        CommandMsg msg;
        while (running_.load(std::memory_order_relaxed)) {
            if (!in.pop(msg)) {
                cpu_relax();
                continue;
            }
            ResultMsg result;
            result.correlation = msg.correlation;
            std::visit(
                [&](auto&& cmd) {
                    using C = std::decay_t<decltype(cmd)>;
                    if constexpr (std::is_same_v<C, AddOrder>) {
                        auto r = engine_.add(cmd, msg.ingress_sequence);
                        result.events = std::move(r.first);
                        result.trades = std::move(r.second);
                    } else if constexpr (std::is_same_v<C, CancelOrder>) {
                        result.events = engine_.cancel(cmd, msg.ingress_sequence);
                    } else {
                        auto r = engine_.replace(cmd, msg.ingress_sequence);
                        result.events = std::move(r.first);
                        result.trades = std::move(r.second);
                    }
                },
                msg.command);
            while (running_.load(std::memory_order_relaxed) && !out.push(std::move(result))) {
                cpu_relax();
            }
        }
    }

    static void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#else
        std::this_thread::yield();
#endif
    }

    static void pin_to_core(std::thread& t, int core) {
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<unsigned>(core), &set);
        pthread_setaffinity_np(t.native_handle(), sizeof(set), &set);
#else
        (void)t;
        (void)core;  // macOS exposes no portable hard-affinity API; best-effort no-op.
#endif
    }

    ShardedMatchingEngine engine_;
    int shard_count_;
    std::vector<std::unique_ptr<SpscQueue<CommandMsg>>> inbound_;
    std::vector<std::unique_ptr<SpscQueue<ResultMsg>>> outbound_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> next_correlation_{0};
};

}  // namespace me
