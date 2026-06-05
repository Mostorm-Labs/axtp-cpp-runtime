#pragma once

#include <cstddef>

#include "model/bytes.hpp"
#include "model/result.hpp"

namespace axtp {

class ByteBuffer {
public:
    void append(const Byte* data, std::size_t size) {
        if (size == 0) {
            return;
        }
        _bytes.insert(_bytes.end(), data, data + size);
    }

    void append(const Bytes& bytes) {
        append(bytes.data(), bytes.size());
    }

    Result<Bytes> peek(std::size_t size) const {
        return peek(0, size);
    }

    Result<Bytes> peek(std::size_t offset, std::size_t size) const {
        if (offset > _bytes.size() || size > _bytes.size() - offset) {
            return Result<Bytes>::failure(kByteIoOutOfRange, "not enough bytes to peek");
        }
        auto begin = _bytes.begin() + static_cast<std::ptrdiff_t>(offset);
        return Result<Bytes>::success(Bytes(begin, begin + static_cast<std::ptrdiff_t>(size)));
    }

    Result<Bytes> consume(std::size_t size) {
        if (size > _bytes.size()) {
            return Result<Bytes>::failure(kByteIoOutOfRange, "not enough bytes to consume");
        }
        Bytes out(_bytes.begin(), _bytes.begin() + static_cast<std::ptrdiff_t>(size));
        _bytes.erase(_bytes.begin(), _bytes.begin() + static_cast<std::ptrdiff_t>(size));
        return Result<Bytes>::success(std::move(out));
    }

    const Bytes& bytes() const {
        return _bytes;
    }

    std::size_t size() const {
        return _bytes.size();
    }

    bool empty() const {
        return _bytes.empty();
    }

    void clear() {
        _bytes.clear();
    }

private:
    Bytes _bytes;
};

}  // namespace axtp
