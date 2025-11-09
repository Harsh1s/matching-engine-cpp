#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <variant>

#include "types.hpp"

namespace me {

using Command = std::variant<AddOrder, CancelOrder, ReplaceOrder>;

struct SequencedCommand {
    long long ingress_sequence = 0;
    Command command;
};

// Bounded FIFO ingress ring. Mirrors the Python deque(maxlen=capacity):
// pushing past capacity drops the oldest entry.
class IngressRing {
public:
    explicit IngressRing(std::size_t capacity = 4096) : capacity_(capacity) {}

    void push(SequencedCommand item) {
        if (queue_.size() == capacity_ && !queue_.empty()) {
            queue_.pop_front();
        }
        queue_.push_back(std::move(item));
    }

    std::optional<SequencedCommand> pop() {
        if (queue_.empty()) return std::nullopt;
        SequencedCommand item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    std::size_t size() const { return queue_.size(); }

private:
    std::size_t capacity_;
    std::deque<SequencedCommand> queue_;
};

// Assigns a strictly increasing ingress sequence number to each command.
class Sequencer {
public:
    SequencedCommand assign(Command command) {
        SequencedCommand msg{next_, std::move(command)};
        ++next_;
        return msg;
    }

private:
    long long next_ = 1;
};

}  // namespace me
