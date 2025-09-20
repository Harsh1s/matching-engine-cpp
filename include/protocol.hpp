#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "types.hpp"

namespace me {

std::string pack_add_order(const AddOrder& cmd);
AddOrder unpack_add_order(const std::string& buf);
std::string pack_cancel_order(const CancelOrder& cmd);
CancelOrder unpack_cancel_order(const std::string& buf);
std::string pack_replace_order(const ReplaceOrder& cmd);
ReplaceOrder unpack_replace_order(const std::string& buf);

}  // namespace me
