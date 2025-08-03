#include <iostream>
#include <string>
#include <vector>

#include "matching_engine.hpp"
#include "types.hpp"

using namespace me;

namespace {
int g_failures = 0;
void check(bool ok, const std::string& name) {
  if (!ok) { ++g_failures; std::cerr << "FAIL: " << name << "\n"; }
}
}  // namespace

int main() {
  MatchingEngine engine;
  engine.add(AddOrder{"NFLX", 1, "A", Side::Sell, 100, 5});
  engine.add(AddOrder{"NFLX", 2, "B", Side::Sell, 100, 5});
  auto [events, trades] = engine.add(AddOrder{"NFLX", 3, "C", Side::Buy, 101, 7});
  check(trades.size() == 2, "ptp: two trades");
  check(trades[0].seller_order_id == 1, "ptp: fifo");
  auto cancel_events = engine.cancel(CancelOrder{"NFLX", 0});
  check(cancel_events.front().type == EventType::Rejected, "cancel unknown");
  if (g_failures) return 1;
  std::cout << "All tests passed.\n";
  return 0;
}
