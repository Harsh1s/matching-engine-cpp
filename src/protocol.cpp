#include "protocol.hpp"

#include <cstdint>
#include <stdexcept>
#include <variant>

#include "checksum.hpp"

namespace me {

namespace {

constexpr std::size_t kHeaderSize = 5 + 8 * 3;  // 5 x u8 + 3 x u64

void put_u8(std::string& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }

void put_u32(std::string& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}

void put_u64(std::string& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}

std::uint8_t get_u8(const std::string& buf, std::size_t offset) {
    return static_cast<std::uint8_t>(buf[offset]);
}

std::uint32_t get_u32(const std::string& buf, std::size_t offset) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v = (v << 8) | static_cast<std::uint8_t>(buf[offset + i]);
    }
    return v;
}

std::uint64_t get_u64(const std::string& buf, std::size_t offset) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint8_t>(buf[offset + i]);
    }
    return v;
}

int tif_to_code(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return 1;
        case TimeInForce::IOC: return 2;
        case TimeInForce::FOK: return 3;
        case TimeInForce::MARKET: return 4;
    }
    return 1;
}

TimeInForce code_to_tif(int code) {
    switch (code) {
        case 1: return TimeInForce::GTC;
        case 2: return TimeInForce::IOC;
        case 3: return TimeInForce::FOK;
        case 4: return TimeInForce::MARKET;
    }
    throw std::runtime_error("invalid tif code");
}

int stp_to_code(SelfTradePrevention stp) {
    switch (stp) {
        case SelfTradePrevention::None: return 0;
        case SelfTradePrevention::CancelAggressor: return 1;
        case SelfTradePrevention::CancelPassive: return 2;
        case SelfTradePrevention::CancelBoth: return 3;
    }
    return 0;
}

SelfTradePrevention code_to_stp(int code) {
    switch (code) {
        case 0: return SelfTradePrevention::None;
        case 1: return SelfTradePrevention::CancelAggressor;
        case 2: return SelfTradePrevention::CancelPassive;
        case 3: return SelfTradePrevention::CancelBoth;
    }
    throw std::runtime_error("invalid stp code");
}

}  // namespace

std::string pack_add_order(const AddOrder& cmd) {
    if (cmd.symbol.size() > 255 || cmd.participant_id.size() > 255) {
        throw std::runtime_error("symbol or participant too long");
    }
    std::string out;
    out.reserve(kHeaderSize + cmd.symbol.size() + cmd.participant_id.size());
    put_u8(out, static_cast<std::uint8_t>(cmd.symbol.size()));
    put_u8(out, static_cast<std::uint8_t>(cmd.participant_id.size()));
    put_u8(out, cmd.side == Side::Buy ? 1 : 2);
    put_u8(out, static_cast<std::uint8_t>(tif_to_code(cmd.time_in_force)));
    put_u8(out, static_cast<std::uint8_t>(stp_to_code(cmd.stp)));
    put_u64(out, static_cast<std::uint64_t>(cmd.order_id));
    put_u64(out, static_cast<std::uint64_t>(cmd.price_ticks));
    put_u64(out, static_cast<std::uint64_t>(cmd.quantity));
    out += cmd.symbol;
    out += cmd.participant_id;
    return out;
}

AddOrder unpack_add_order(const std::string& buf) {
    if (buf.size() < kHeaderSize) throw std::runtime_error("buffer too small");
    if (buf.size() > 4096) throw std::runtime_error("buffer too large");
    std::uint8_t symbol_len = get_u8(buf, 0);
    std::uint8_t participant_len = get_u8(buf, 1);
    std::uint8_t side_raw = get_u8(buf, 2);
    std::uint8_t tif_raw = get_u8(buf, 3);
    std::uint8_t stp_raw = get_u8(buf, 4);
    std::uint64_t order_id = get_u64(buf, 5);
    std::uint64_t price_ticks = get_u64(buf, 13);
    std::uint64_t qty = get_u64(buf, 21);

    std::size_t offset = kHeaderSize;
    if (buf.size() < offset + symbol_len + participant_len) {
        throw std::runtime_error("truncated payload");
    }
    AddOrder cmd;
    cmd.symbol = buf.substr(offset, symbol_len);
    offset += symbol_len;
    cmd.participant_id = buf.substr(offset, participant_len);
    cmd.order_id = static_cast<long long>(order_id);
    cmd.side = side_raw == 1 ? Side::Buy : Side::Sell;
    cmd.price_ticks = static_cast<long long>(price_ticks);
    cmd.quantity = static_cast<long long>(qty);
    cmd.time_in_force = code_to_tif(tif_raw);
    cmd.stp = code_to_stp(stp_raw);
    return cmd;
}

std::string pack_cancel_order(const CancelOrder& cmd) {
    if (cmd.symbol.size() > 255) throw std::runtime_error("symbol too long");
    std::string out;
    out.reserve(1 + 8 + cmd.symbol.size());
    put_u8(out, static_cast<std::uint8_t>(cmd.symbol.size()));
    put_u64(out, static_cast<std::uint64_t>(cmd.order_id));
    out += cmd.symbol;
    return out;
}

CancelOrder unpack_cancel_order(const std::string& buf) {
    if (buf.size() < 9) throw std::runtime_error("buffer too small");
    std::uint8_t symbol_len = get_u8(buf, 0);
    std::uint64_t order_id = get_u64(buf, 1);
    if (buf.size() < 9u + symbol_len) throw std::runtime_error("truncated payload");
    CancelOrder cmd;
    cmd.order_id = static_cast<long long>(order_id);
    cmd.symbol = buf.substr(9, symbol_len);
    return cmd;
}

std::string pack_replace_order(const ReplaceOrder& cmd) {
    if (cmd.symbol.size() > 255) throw std::runtime_error("symbol too long");
    std::string out;
    out.reserve(2 + 8 * 4 + cmd.symbol.size());
    put_u8(out, static_cast<std::uint8_t>(cmd.symbol.size()));
    put_u8(out, static_cast<std::uint8_t>(tif_to_code(cmd.time_in_force)));
    put_u64(out, static_cast<std::uint64_t>(cmd.order_id));
    put_u64(out, static_cast<std::uint64_t>(cmd.new_order_id));
    put_u64(out, static_cast<std::uint64_t>(cmd.new_price_ticks));
    put_u64(out, static_cast<std::uint64_t>(cmd.new_quantity));
    out += cmd.symbol;
    return out;
}

ReplaceOrder unpack_replace_order(const std::string& buf) {
    constexpr std::size_t head = 2 + 8 * 4;
    if (buf.size() < head) throw std::runtime_error("buffer too small");
    std::uint8_t symbol_len = get_u8(buf, 0);
    std::uint8_t tif_raw = get_u8(buf, 1);
    if (buf.size() < head + symbol_len) throw std::runtime_error("truncated payload");
    ReplaceOrder cmd;
    cmd.order_id = static_cast<long long>(get_u64(buf, 2));
    cmd.new_order_id = static_cast<long long>(get_u64(buf, 10));
    cmd.new_price_ticks = static_cast<long long>(get_u64(buf, 18));
    cmd.new_quantity = static_cast<long long>(get_u64(buf, 26));
    cmd.time_in_force = code_to_tif(tif_raw);
    cmd.symbol = buf.substr(head, symbol_len);
    return cmd;
}

std::string frame_message(MessageType type, const std::string& payload) {
    std::string out;
    out.reserve(1 + 4 + payload.size() + 4);
    put_u8(out, static_cast<std::uint8_t>(type));
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    out += payload;
    put_u32(out, crc32(payload));
    return out;
}

bool parse_frame(const std::string& data, std::size_t& offset, MessageType& type, std::string& payload) {
    if (data.size() - offset < 5) return false;
    std::size_t p = offset;
    std::uint8_t t = get_u8(data, p);
    std::uint32_t len = get_u32(data, p + 1);
    p += 5;
    if (data.size() - p < static_cast<std::size_t>(len) + 4u) return false;
    std::string body = data.substr(p, len);
    p += len;
    std::uint32_t crc = get_u32(data, p);
    p += 4;
    if (crc32(body) != crc) throw std::runtime_error("frame crc mismatch");
    type = static_cast<MessageType>(t);
    payload = std::move(body);
    offset = p;
    return true;
}

std::string encode_command(const Command& command) {
    return std::visit(
        [](const auto& c) -> std::string {
            using C = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<C, AddOrder>) {
                return frame_message(MessageType::Add, pack_add_order(c));
            } else if constexpr (std::is_same_v<C, CancelOrder>) {
                return frame_message(MessageType::Cancel, pack_cancel_order(c));
            } else {
                return frame_message(MessageType::Replace, pack_replace_order(c));
            }
        },
        command);
}

Command decode_command(MessageType type, const std::string& payload) {
    switch (type) {
        case MessageType::Add: return unpack_add_order(payload);
        case MessageType::Cancel: return unpack_cancel_order(payload);
        case MessageType::Replace: return unpack_replace_order(payload);
        default: throw std::runtime_error("unsupported command message type");
    }
}

}  // namespace me
