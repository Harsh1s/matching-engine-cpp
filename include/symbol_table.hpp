#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace me {

// Interns symbol strings to dense 32-bit ids so the hot path and wire format can
// carry a fixed-size integer instead of a heap string. Single-writer: intern()
// is expected to be called from the gateway thread only.
class SymbolTable {
public:
    std::uint32_t intern(const std::string& symbol) {
        auto it = ids_.find(symbol);
        if (it != ids_.end()) return it->second;
        std::uint32_t id = static_cast<std::uint32_t>(names_.size());
        names_.push_back(symbol);
        ids_.emplace(symbol, id);
        return id;
    }

    bool lookup(const std::string& symbol, std::uint32_t& out) const {
        auto it = ids_.find(symbol);
        if (it == ids_.end()) return false;
        out = it->second;
        return true;
    }

    const std::string& name(std::uint32_t id) const { return names_.at(id); }

    std::size_t size() const { return names_.size(); }

private:
    std::unordered_map<std::string, std::uint32_t> ids_;
    std::vector<std::string> names_;
};

}  // namespace me
