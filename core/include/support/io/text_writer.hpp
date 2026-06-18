#pragma once

#include <cstddef>

namespace axtp {

class ITextWriter {
public:
    virtual ~ITextWriter() = default;
    virtual void writeText(const char* data, std::size_t size) = 0;
};

}  // namespace axtp
