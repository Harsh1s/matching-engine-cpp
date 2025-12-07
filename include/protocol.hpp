#pragma once

#include <string>

#include "types.hpp"

namespace me {

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

}  // namespace me
