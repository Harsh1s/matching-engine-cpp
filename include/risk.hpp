#pragma once

#include <optional>
#include <string>

#include "types.hpp"

namespace me {

struct RiskLimits {
    long long max_order_quantity = 1'000'000;
    long long min_price_ticks = 1;
    long long max_price_ticks = 10'000'000'000LL;
    long long max_notional_ticks = 10'000'000'000'000LL;
};

// Pre-trade risk checks applied before an order reaches the matching core.
class PreTradeRisk {
public:
    PreTradeRisk() = default;
    explicit PreTradeRisk(const RiskLimits& limits) : limits_(limits) {}

    // Returns a rejection reason, or std::nullopt when the order passes.
    std::optional<std::string> check_add(const AddOrder& cmd) const {
        if (cmd.quantity <= 0) return std::string("RISK_QTY_NON_POSITIVE");
        if (cmd.quantity > limits_.max_order_quantity) return std::string("RISK_QTY_TOO_LARGE");
        if (cmd.time_in_force != TimeInForce::MARKET) {
            if (cmd.price_ticks < limits_.min_price_ticks || cmd.price_ticks > limits_.max_price_ticks) {
                return std::string("RISK_PRICE_OUT_OF_RANGE");
            }
            if (cmd.price_ticks * cmd.quantity > limits_.max_notional_ticks) {
                return std::string("RISK_NOTIONAL_EXCEEDED");
            }
        }
        return std::nullopt;
    }

private:
    RiskLimits limits_{};
};

}  // namespace me
