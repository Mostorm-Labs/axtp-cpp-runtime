#pragma once

#include <cstddef>
#include <cstdint>

#include "model/bytes.hpp"

namespace axtp {

inline std::uint16_t crc16CcittFalse(const Byte* data, std::size_t size) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000) != 0) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

inline std::uint16_t crc16CcittFalse(const Bytes& bytes) {
    return crc16CcittFalse(bytes.data(), bytes.size());
}

inline std::uint16_t crc16Xmodem(const Byte* data, std::size_t size) {
    std::uint16_t crc = 0x0000;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000) != 0) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

inline std::uint16_t crc16Xmodem(const Bytes& bytes) {
    return crc16Xmodem(bytes.data(), bytes.size());
}

}  // namespace axtp
