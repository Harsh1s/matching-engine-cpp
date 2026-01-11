#include "recovery.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "checksum.hpp"

namespace me {

namespace {

constexpr std::size_t kCommitBatch = 64;  // group-commit window (records)


void ensure_directory(const std::string& root) {
    // Create each path component if missing (mkdir -p, single-level safe).
    std::string accumulated;
    for (std::size_t i = 0; i <= root.size(); ++i) {
        if (i == root.size() || root[i] == '/') {
            if (!accumulated.empty()) {
                ::mkdir(accumulated.c_str(), 0755);
            }
            if (i < root.size()) accumulated.push_back('/');
        } else {
            accumulated.push_back(root[i]);
        }
    }
}

}  // namespace

WalStore::WalStore(const std::string& root, int shard_id, AckPolicy ack_policy)
    : ack_policy_(ack_policy) {
    ensure_directory(root);
    wal_path_ = root + "/shard_" + std::to_string(shard_id) + ".wal.jsonl";
    snapshot_path_ = root + "/shard_" + std::to_string(shard_id) + ".snapshot.json";
    handle_ = std::fopen(wal_path_.c_str(), "ab");
    if (handle_ == nullptr) {
        throw std::runtime_error("failed to open WAL: " + wal_path_);
    }
}

WalStore::~WalStore() { close(); }

WalStore::WalStore(WalStore&& other) noexcept
    : wal_path_(std::move(other.wal_path_)),
      snapshot_path_(std::move(other.snapshot_path_)),
      ack_policy_(other.ack_policy_),
      handle_(other.handle_),
      pending_(std::move(other.pending_)),
      batch_count_(other.batch_count_) {
    other.handle_ = nullptr;
    other.batch_count_ = 0;
}

WalStore& WalStore::operator=(WalStore&& other) noexcept {
    if (this != &other) {
        close();
        wal_path_ = std::move(other.wal_path_);
        snapshot_path_ = std::move(other.snapshot_path_);
        ack_policy_ = other.ack_policy_;
        handle_ = other.handle_;
        pending_ = std::move(other.pending_);
        batch_count_ = other.batch_count_;
        other.handle_ = nullptr;
        other.batch_count_ = 0;
    }
    return *this;
}

void WalStore::append(const Json& command) {
    // Each record is "<crc32:8hex> <json>\n" so replay can detect a torn tail.
    std::string json = command.dump();
    char prefix[9];
    std::snprintf(prefix, sizeof(prefix), "%08x", crc32(json));
    pending_.append(prefix, 8);
    pending_.push_back(' ');
    pending_ += json;
    pending_.push_back('\n');
    if (++batch_count_ >= kCommitBatch) flush();
}

void WalStore::flush() {
    if (handle_ == nullptr) return;
    if (!pending_.empty()) {
        std::fwrite(pending_.data(), 1, pending_.size(), handle_);
        pending_.clear();
    }
    batch_count_ = 0;
    std::fflush(handle_);
    if (ack_policy_ == AckPolicy::Fsync) ::fsync(fileno(handle_));
}

std::vector<Json> WalStore::replay_commands() {
    flush();
    std::vector<Json> commands;
    std::ifstream in(wal_path_, std::ios::binary);
    if (!in) return commands;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 9 || line[8] != ' ') continue;
        std::uint32_t expected =
            static_cast<std::uint32_t>(std::strtoul(line.substr(0, 8).c_str(), nullptr, 16));
        std::string json = line.substr(9);
        if (crc32(json) != expected) break;  // truncated/corrupt tail: stop here
        commands.push_back(Json::parse(json));
    }
    return commands;
}

void WalStore::truncate_wal() {
    if (handle_ != nullptr) std::fclose(handle_);
    handle_ = std::fopen(wal_path_.c_str(), "wb");
    if (handle_ == nullptr) throw std::runtime_error("failed to truncate WAL: " + wal_path_);
    pending_.clear();
    batch_count_ = 0;
}

void WalStore::save_snapshot(const Json& snapshot) {
    flush();
    std::string payload = snapshot.dump();
    std::string tmp = snapshot_path_ + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) throw std::runtime_error("failed to open snapshot temp: " + tmp);
    std::fwrite(payload.data(), 1, payload.size(), f);
    std::fflush(f);
    ::fsync(fileno(f));
    std::fclose(f);
    if (std::rename(tmp.c_str(), snapshot_path_.c_str()) != 0) {
        throw std::runtime_error("failed to commit snapshot: " + snapshot_path_);
    }
    truncate_wal();  // snapshot supersedes everything written so far
}

std::optional<Json> WalStore::load_snapshot() {
    std::ifstream in(snapshot_path_, std::ios::binary);
    if (!in) return std::nullopt;
    std::string payload((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (payload.empty()) return std::nullopt;
    return Json::parse(payload);
}

void WalStore::close() {
    if (handle_ != nullptr) {
        flush();
        std::fclose(handle_);
        handle_ = nullptr;
    }
}

}  // namespace me
