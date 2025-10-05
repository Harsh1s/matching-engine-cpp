#pragma once

// Minimal dependency-free JSON value used for the WAL (JSON-lines) and
// snapshot persistence, mirroring the Python engine's recovery format.
// Objects preserve insertion order so snapshots serialize deterministically.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace me {

class Json {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Type type = Type::Null;
    bool bool_value = false;
    long long int_value = 0;
    double double_value = 0.0;
    std::string string_value;
    std::vector<Json> array_value;
    std::vector<std::pair<std::string, Json>> object_value;

    Json() = default;

    static Json make_object() {
        Json j;
        j.type = Type::Object;
        return j;
    }
    static Json make_array() {
        Json j;
        j.type = Type::Array;
        return j;
    }
    static Json from_int(long long value) {
        Json j;
        j.type = Type::Int;
        j.int_value = value;
        return j;
    }
    static Json from_string(const std::string& value) {
        Json j;
        j.type = Type::String;
        j.string_value = value;
        return j;
    }

    void set(const std::string& key, Json value) {
        object_value.emplace_back(key, std::move(value));
    }
    void push_back(Json value) { array_value.push_back(std::move(value)); }

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }

    // Returns nullptr when the key is absent.
    const Json* find(const std::string& key) const {
        for (const auto& kv : object_value) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    long long as_int() const { return type == Type::Int ? int_value : static_cast<long long>(double_value); }
    std::string as_string() const { return string_value; }

    std::string dump() const;
    static Json parse(const std::string& text);
};

}  // namespace me
