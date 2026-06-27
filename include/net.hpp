#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "gateway.hpp"

namespace me {

// Poll-based TCP order-entry gateway. Accepts connections, reassembles framed
// commands from each stream, and invokes the handler for every decoded command.
// The handler runs on the server's I/O thread and is expected to hand the
// command off to the matching pipeline (e.g. ThreadedShardedEngine::submit).
class OrderEntryServer {
public:
    using Handler = std::function<void(const Command&)>;

    OrderEntryServer(std::uint16_t port, Handler handler);
    ~OrderEntryServer();

    OrderEntryServer(const OrderEntryServer&) = delete;
    OrderEntryServer& operator=(const OrderEntryServer&) = delete;

    void start();
    void stop();

    // Actual bound port (useful when constructed with port 0 for an ephemeral).
    std::uint16_t port() const { return port_; }

private:
    struct Conn {
        int fd = -1;
        std::string buffer;
    };

    void run();
    void drain_connection(Conn& conn);

    std::uint16_t port_;
    Handler handler_;
    int listen_fd_ = -1;
    std::vector<Conn> conns_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

// UDP multicast publisher for the outbound market-data / execution feed.
class MulticastPublisher {
public:
    MulticastPublisher(const std::string& group, std::uint16_t port, std::uint8_t ttl = 1);
    ~MulticastPublisher();

    MulticastPublisher(const MulticastPublisher&) = delete;
    MulticastPublisher& operator=(const MulticastPublisher&) = delete;

    void publish(const std::string& bytes);

private:
    int fd_ = -1;
    std::uint16_t port_;
    std::uint32_t group_addr_;  // network byte order
};

}  // namespace me
