#include "net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "protocol.hpp"

namespace me {

namespace {

void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

OrderEntryServer::OrderEntryServer(std::uint16_t port, Handler handler)
    : port_(port), handler_(std::move(handler)) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(listen_fd_);
        throw std::runtime_error("bind() failed");
    }
    if (::listen(listen_fd_, 64) != 0) {
        ::close(listen_fd_);
        throw std::runtime_error("listen() failed");
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port_ = ntohs(addr.sin_port);
    }
    set_nonblocking(listen_fd_);
}

OrderEntryServer::~OrderEntryServer() {
    stop();
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void OrderEntryServer::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void OrderEntryServer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    for (auto& c : conns_) {
        if (c.fd >= 0) ::close(c.fd);
    }
    conns_.clear();
}

void OrderEntryServer::drain_connection(Conn& conn) {
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(conn.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            conn.buffer.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            ::close(conn.fd);
            conn.fd = -1;
            return;
        }
        break;  // EAGAIN / EWOULDBLOCK: no more data for now
    }

    std::size_t offset = 0;
    MessageType type;
    std::string payload;
    try {
        while (parse_frame(conn.buffer, offset, type, payload)) {
            if (type != MessageType::Event) handler_(decode_command(type, payload));
        }
    } catch (const std::exception&) {
        ::close(conn.fd);  // malformed frame: drop the session
        conn.fd = -1;
        return;
    }
    if (offset > 0) conn.buffer.erase(0, offset);
}

void OrderEntryServer::run() {
    std::vector<pollfd> fds;
    while (running_.load(std::memory_order_relaxed)) {
        fds.clear();
        fds.push_back(pollfd{listen_fd_, POLLIN, 0});
        for (auto& c : conns_) {
            if (c.fd >= 0) fds.push_back(pollfd{c.fd, POLLIN, 0});
        }

        int ready = ::poll(fds.data(), fds.size(), 100);
        if (ready <= 0) continue;

        if (fds[0].revents & POLLIN) {
            int client = ::accept(listen_fd_, nullptr, nullptr);
            while (client >= 0) {
                set_nonblocking(client);
                conns_.push_back(Conn{client, std::string()});
                client = ::accept(listen_fd_, nullptr, nullptr);
            }
        }

        for (std::size_t i = 1; i < fds.size(); ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                for (auto& c : conns_) {
                    if (c.fd == fds[i].fd) {
                        drain_connection(c);
                        break;
                    }
                }
            }
        }

        conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                    [](const Conn& c) { return c.fd < 0; }),
                     conns_.end());
    }
}

MulticastPublisher::MulticastPublisher(const std::string& group, std::uint16_t port, std::uint8_t ttl)
    : port_(port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket() failed");
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    in_addr addr{};
    if (::inet_pton(AF_INET, group.c_str(), &addr) != 1) {
        ::close(fd_);
        throw std::runtime_error("invalid multicast group: " + group);
    }
    group_addr_ = addr.s_addr;
}

MulticastPublisher::~MulticastPublisher() {
    if (fd_ >= 0) ::close(fd_);
}

void MulticastPublisher::publish(const std::string& bytes) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = group_addr_;
    dst.sin_port = htons(port_);
    ::sendto(fd_, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

}  // namespace me
