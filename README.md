# matching-engine-cpp

Deterministic price-time-priority matching engine in C++17.

## Features

- Integer tick prices and quantities
- Price-time priority with passive-price execution
- Time-in-force: GTC, IOC, FOK, MARKET
- Cancel and replace
- Self-trade prevention
- Pre-trade risk controls
- Binary add/cancel/replace codec
- Symbol sharding with per-shard WAL, snapshot, and replay
- Ingress sequencing via gateway sequencer

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```
