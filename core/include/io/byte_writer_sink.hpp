#pragma once

#include <cstddef>

#include "model/bytes.hpp"

namespace axtp {

class IByteWriter {
public:
    virtual ~IByteWriter() = default;
    virtual void writeBytes(const Byte* data, std::size_t size) = 0;
};

}  // namespace axtp
