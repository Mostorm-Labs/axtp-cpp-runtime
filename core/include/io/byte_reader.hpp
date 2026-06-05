#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "model/bytes.hpp"
#include "model/result.hpp"

namespace axtp {

class ByteReader {
public:
    explicit ByteReader(const Bytes& bytes)
        : _data(bytes.data())
        , _size(bytes.size()) {}

    ByteReader(const Byte* data, std::size_t size)
        : _data(data)
        , _size(size) {}

    Result<std::uint8_t> readU8() {
        if (!hasRemaining(1)) {
            return Result<std::uint8_t>::failure(kByteIoOutOfRange, "not enough bytes for u8");
        }
        return Result<std::uint8_t>::success(_data[_offset++]);
    }

    Result<std::uint16_t> readU16() {
        if (!hasRemaining(2)) {
            return Result<std::uint16_t>::failure(kByteIoOutOfRange, "not enough bytes for u16");
        }
        std::uint16_t value = static_cast<std::uint16_t>(_data[_offset]) |
                              static_cast<std::uint16_t>(_data[_offset + 1] << 8);
        _offset += 2;
        return Result<std::uint16_t>::success(value);
    }

    Result<std::uint32_t> readU32() {
        if (!hasRemaining(4)) {
            return Result<std::uint32_t>::failure(kByteIoOutOfRange, "not enough bytes for u32");
        }
        std::uint32_t value = static_cast<std::uint32_t>(_data[_offset]) |
                              (static_cast<std::uint32_t>(_data[_offset + 1]) << 8) |
                              (static_cast<std::uint32_t>(_data[_offset + 2]) << 16) |
                              (static_cast<std::uint32_t>(_data[_offset + 3]) << 24);
        _offset += 4;
        return Result<std::uint32_t>::success(value);
    }

    Result<std::uint64_t> readU64() {
        if (!hasRemaining(8)) {
            return Result<std::uint64_t>::failure(kByteIoOutOfRange, "not enough bytes for u64");
        }
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(_data[_offset++]) << shift;
        }
        return Result<std::uint64_t>::success(value);
    }

    Result<Bytes> readBytes(std::size_t size) {
        if (!hasRemaining(size)) {
            return Result<Bytes>::failure(kByteIoOutOfRange, "not enough bytes");
        }
        Bytes out(_data + _offset, _data + _offset + size);
        _offset += size;
        return Result<Bytes>::success(std::move(out));
    }

    bool hasRemaining(std::size_t count) const {
        return count <= remaining();
    }

    std::size_t offset() const {
        return _offset;
    }

    std::size_t remaining() const {
        return _size - _offset;
    }

    bool empty() const {
        return remaining() == 0;
    }

private:
    const Byte* _data = nullptr;
    std::size_t _size = 0;
    std::size_t _offset = 0;
};

}  // namespace axtp
