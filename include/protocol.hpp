#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "gateway.hpp"
#include "types.hpp"

namespace me {

enum class MessageType : std::uint8_t { Add = 1, Cancel = 2, Replace = 3, Event = 10 };

// Compact fixed-header big-endian wire format for add-order commands:
//   symbol_len:u8, participant_len:u8, side:u8, tif:u8, stp:u8,
//   order_id:u64, price:u64, qty:u64, then symbol + participant bytes.
std::string pack_add_order(const AddOrder& cmd);
AddOrder unpack_add_order(const std::string& buf);

// symbol_len:u8, order_id:u64, then symbol bytes.
std::string pack_cancel_order(const CancelOrder& cmd);
CancelOrder unpack_cancel_order(const std::string& buf);

// symbol_len:u8, tif:u8, order_id:u64, new_order_id:u64, new_price:u64,
// new_qty:u64, then symbol bytes.
std::string pack_replace_order(const ReplaceOrder& cmd);
ReplaceOrder unpack_replace_order(const std::string& buf);

// Self-describing frame for stream transports:
//   type:u8, length:u32, payload[length], crc32:u32  (all big-endian).
std::string frame_message(MessageType type, const std::string& payload);

// Extracts one frame from `data` at `offset`. Returns false when fewer than a
// full frame's bytes have arrived (offset untouched); on success advances
// `offset` past the frame. Throws on a CRC mismatch.
bool parse_frame(const std::string& data, std::size_t& offset, MessageType& type, std::string& payload);

// Encode a command into a single framed message; decode the inverse.
std::string encode_command(const Command& command);
Command decode_command(MessageType type, const std::string& payload);

}  // namespace me
