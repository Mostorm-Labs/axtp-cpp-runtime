#pragma once

#include <cstddef>

#include "support/io/byte_sink.hpp"
#include "protocol/model/bytes.hpp"
#include "runtime/transport/transport_profile.hpp"

namespace axtp {

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void bind(IByteSink& sink) = 0;
    virtual void open() = 0;
    virtual void close() = 0;

    virtual void poll() {}

    virtual void sendBytes(const Byte* data, std::size_t size) = 0;
    virtual TransportProfile profile() const = 0;
};

}  // namespace axtp
