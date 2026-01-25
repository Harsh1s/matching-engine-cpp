#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace me {

#if defined(__cpp_lib_hardware_interference_size)
constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLine = 64;
#endif

// Bounded, wait-free single-producer single-consumer ring buffer.
//
// One thread calls push(), one thread calls pop(); no locks. head_ and tail_
// live on separate cache lines to avoid false sharing. Capacity is rounded up
// to a power of two so index wrap is a mask instead of a modulo.
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity)
        : capacity_(round_up_pow2(capacity)), mask_(capacity_ - 1), slots_(capacity_) {}

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Returns false (without storing) when the queue is full.
    bool push(T value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > capacity_) {
            return false;
        }
        slots_[head & mask_] = std::move(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Returns false (leaving out untouched) when the queue is empty.
    bool pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = std::move(slots_[tail & mask_]);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    std::size_t capacity() const { return capacity_; }

private:
    static std::size_t round_up_pow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<T> slots_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
};

}  // namespace me
