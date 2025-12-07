#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace me {

// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). Used for WAL torn-write
// detection and network frame integrity. Table built once on first use.
inline std::uint32_t crc32(const void* data, std::size_t len, std::uint32_t seed = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = ~seed;
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

inline std::uint32_t crc32(const std::string& s, std::uint32_t seed = 0) {
    return crc32(s.data(), s.size(), seed);
}

}  // namespace me
