#pragma once

#include <cstddef>

#include "model/bytes.hpp"

namespace axtp {

enum class TransportPacketKind {
    ByteStreamChunk,
    BinaryMessage,
    TextMessage,
};

struct TransportPacket {
    TransportPacketKind kind = TransportPacketKind::ByteStreamChunk;
    const Byte* data = nullptr;
    std::size_t size = 0;
};

class ITransportSink {
public:
    virtual ~ITransportSink() = default;
    virtual void onTransportPacket(const TransportPacket& packet) = 0;
};

}  // namespace axtp
