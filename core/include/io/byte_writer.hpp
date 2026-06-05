#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "model/bytes.hpp"

namespace axtp {

class ByteWriter {
public:
    void writeU8(std::uint8_t value) {
        _bytes.push_back(value);
    }

    void writeU16(std::uint16_t value) {
        _bytes.push_back(static_cast<Byte>(value & 0xFF));
        _bytes.push_back(static_cast<Byte>((value >> 8) & 0xFF));
    }

    void writeU32(std::uint32_t value) {
        _bytes.push_back(static_cast<Byte>(value & 0xFF));
        _bytes.push_back(static_cast<Byte>((value >> 8) & 0xFF));
        _bytes.push_back(static_cast<Byte>((value >> 16) & 0xFF));
        _bytes.push_back(static_cast<Byte>((value >> 24) & 0xFF));
    }

    void writeU64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            _bytes.push_back(static_cast<Byte>((value >> shift) & 0xFF));
        }
    }

    void writeBytes(const Byte* data, std::size_t size) {
        if (size == 0) {
            return;
        }
        _bytes.insert(_bytes.end(), data, data + size);
    }

    void writeBytes(const Bytes& bytes) {
        writeBytes(bytes.data(), bytes.size());
    }

    const Bytes& bytes() const {
        return _bytes;
    }

    Bytes takeBytes() {
        Bytes out = std::move(_bytes);
        _bytes.clear();
        return out;
    }

    void clear() {
        _bytes.clear();
    }

private:
    Bytes _bytes;
};

}  // namespace axtp
