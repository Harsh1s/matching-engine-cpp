#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>

namespace me {

enum class Side { Buy, Sell };

inline Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

inline const char* to_string(Side side) { return side == Side::Buy ? "BUY" : "SELL"; }

inline Side side_from_string(const std::string& value) {
    return value == "BUY" ? Side::Buy : Side::Sell;
}

enum class TimeInForce { GTC, IOC, FOK, MARKET };

inline const char* to_string(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::MARKET: return "MARKET";
    }
    return "GTC";
}

inline TimeInForce tif_from_string(const std::string& value) {
    if (value == "IOC") return TimeInForce::IOC;
    if (value == "FOK") return TimeInForce::FOK;
    if (value == "MARKET") return TimeInForce::MARKET;
    return TimeInForce::GTC;
}

enum class SelfTradePrevention { None, CancelAggressor, CancelPassive, CancelBoth };

inline const char* to_string(SelfTradePrevention stp) {
    switch (stp) {
        case SelfTradePrevention::None: return "NONE";
        case SelfTradePrevention::CancelAggressor: return "CANCEL_AGGRESSOR";
        case SelfTradePrevention::CancelPassive: return "CANCEL_PASSIVE";
        case SelfTradePrevention::CancelBoth: return "CANCEL_BOTH";
    }
    return "NONE";
}

inline SelfTradePrevention stp_from_string(const std::string& value) {
    if (value == "CANCEL_AGGRESSOR") return SelfTradePrevention::CancelAggressor;
    if (value == "CANCEL_PASSIVE") return SelfTradePrevention::CancelPassive;
    if (value == "CANCEL_BOTH") return SelfTradePrevention::CancelBoth;
    return SelfTradePrevention::None;
}

enum class EventType { Accepted, Executed, Rested, Cancelled, Rejected };

inline const char* to_string(EventType type) {
    switch (type) {
        case EventType::Accepted: return "ACCEPTED";
        case EventType::Executed: return "EXECUTED";
        case EventType::Rested: return "RESTED";
        case EventType::Cancelled: return "CANCELLED";
        case EventType::Rejected: return "REJECTED";
    }
    return "ACCEPTED";
}

struct AddOrder {
    std::string symbol;
    long long order_id = 0;
    std::string participant_id;
    Side side = Side::Buy;
    long long price_ticks = 0;
    long long quantity = 0;
    TimeInForce time_in_force = TimeInForce::GTC;
    SelfTradePrevention stp = SelfTradePrevention::None;
};

struct CancelOrder {
    std::string symbol;
    long long order_id = 0;
};

struct ReplaceOrder {
    std::string symbol;
    long long order_id = 0;
    long long new_order_id = 0;
    long long new_price_ticks = 0;
    long long new_quantity = 0;
    TimeInForce time_in_force = TimeInForce::GTC;
};

// Value-semantic trade record, mirroring the Python NamedTuple.
struct Trade {
    std::string symbol;
    long long price_ticks = 0;
    long long quantity = 0;
    long long buyer_order_id = 0;
    long long seller_order_id = 0;
    std::string buyer_participant_id;
    std::string seller_participant_id;
    long long passive_order_id = 0;
    long long aggressive_order_id = 0;
};

// Payload values are either an integer or a string, mirroring the
// Python dict[str, int | str] used on the hot path.
using PayloadValue = std::variant<long long, std::string>;
using Payload = std::map<std::string, PayloadValue>;

struct ExecEvent {
    long long sequence = 0;
    EventType type = EventType::Accepted;
    std::string symbol;
    Payload payload;
};

inline long long payload_int(const Payload& payload, const std::string& key) {
    return std::get<long long>(payload.at(key));
}

inline std::string payload_str(const Payload& payload, const std::string& key) {
    return std::get<std::string>(payload.at(key));
}

}  // namespace me
