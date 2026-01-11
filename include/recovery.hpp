#pragma once

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace me {

enum class AckPolicy { Async, Fsync };

// Append-only WAL plus snapshot persistence for a single shard.
// WAL is JSON-lines; snapshot is a single JSON document.
class WalStore {
public:
    WalStore(const std::string& root, int shard_id, AckPolicy ack_policy = AckPolicy::Async);
    ~WalStore();

    WalStore(const WalStore&) = delete;
    WalStore& operator=(const WalStore&) = delete;
    WalStore(WalStore&&) noexcept;
    WalStore& operator=(WalStore&&) noexcept;

    void append(const Json& command);
    std::vector<Json> replay_commands();
    // Writes the snapshot atomically (temp file + rename + fsync) and truncates
    // the WAL, so recovery is snapshot + the records appended after it.
    void save_snapshot(const Json& snapshot);
    std::optional<Json> load_snapshot();
    // Forces any buffered WAL records to the OS (and to disk under Fsync).
    void flush();
    void close();

private:
    std::string wal_path_;
    std::string snapshot_path_;
    AckPolicy ack_policy_ = AckPolicy::Async;
    std::FILE* handle_ = nullptr;
    std::string pending_;        // group-commit staging buffer
    std::size_t batch_count_ = 0;

    void truncate_wal();
};

}  // namespace me
