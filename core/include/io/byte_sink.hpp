#pragma once

#include <cstddef>

#include "model/bytes.hpp"

namespace axtp {

class IByteSink {
public:
    virtual ~IByteSink() = default;
    virtual void onBytes(const Byte* data, std::size_t size) = 0;
};

}  // namespace axtp
