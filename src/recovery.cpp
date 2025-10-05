#include "recovery.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace me {

namespace {

void ensure_directory(const std::string& root) {
    std::string accumulated;
    for (std::size_t i = 0; i <= root.size(); ++i) {
        if (i == root.size() || root[i] == '/') {
            if (!accumulated.empty()) ::mkdir(accumulated.c_str(), 0755);
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
    if (handle_ == nullptr) throw std::runtime_error("failed to open WAL: " + wal_path_);
}

WalStore::~WalStore() { close(); }

WalStore::WalStore(WalStore&& other) noexcept
    : wal_path_(std::move(other.wal_path_)),
      snapshot_path_(std::move(other.snapshot_path_)),
      ack_policy_(other.ack_policy_),
      handle_(other.handle_) {
    other.handle_ = nullptr;
}

WalStore& WalStore::operator=(WalStore&& other) noexcept {
    if (this != &other) {
        close();
        wal_path_ = std::move(other.wal_path_);
        snapshot_path_ = std::move(other.snapshot_path_);
        ack_policy_ = other.ack_policy_;
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void WalStore::append(const Json& command) {
    std::string line = command.dump();
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), handle_);
    std::fflush(handle_);
}

std::vector<Json> WalStore::replay_commands() {
    std::vector<Json> out;
    std::ifstream in(wal_path_);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        out.push_back(Json::parse(line));
    }
    return out;
}

void WalStore::save_snapshot(const Json& snapshot) {
    std::ofstream out(snapshot_path_);
    out << snapshot.dump();
}

std::optional<Json> WalStore::load_snapshot() {
    std::ifstream in(snapshot_path_);
    if (!in) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (content.empty()) return std::nullopt;
    return Json::parse(content);
}

void WalStore::close() {
    if (handle_ != nullptr) {
        std::fclose(handle_);
        handle_ = nullptr;
    }
}

}  // namespace me
