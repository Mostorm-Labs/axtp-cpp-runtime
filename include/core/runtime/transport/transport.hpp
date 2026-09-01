#pragma once

#include <cstddef>
#include <cstdint>

#include "core/support/io/byte_sink.hpp"
#include "core/protocol/model/bytes.hpp"
#include "core/runtime/transport/transport_profile.hpp"

namespace axtp {

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void bind(IByteSink& sink) = 0;
    virtual void open() = 0;
    virtual void close() = 0;

    virtual void poll() {}

    virtual void sendBytes(const Byte* data, std::size_t size) = 0;
    virtual std::uint64_t currentReplyTarget() const { return 0; }
    virtual void sendBytesTo(std::uint64_t replyTarget, const Byte* data, std::size_t size) {
        (void)replyTarget;
        sendBytes(data, size);
    }
    virtual TransportProfile profile() const = 0;
};

}  // namespace axtp
