#include <iostream>

#include "matching_engine.hpp"
#include "types.hpp"

using namespace me;

int main() {
  MatchingEngine engine;
  engine.add(AddOrder{"NFLX", 1, "A", Side::Sell, 100, 5});
  engine.add(AddOrder{"NFLX", 2, "B", Side::Sell, 100, 5});
  auto [events, trades] = engine.add(AddOrder{"NFLX", 3, "C", Side::Buy, 101, 7});
  std::cout << "events=" << events.size() << " trades=" << trades.size() << "\n";
  return 0;
}
