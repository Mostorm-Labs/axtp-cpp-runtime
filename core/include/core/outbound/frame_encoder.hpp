#pragma once

#include <cstdint>

#include "io/byte_writer.hpp"
#include "io/crc16.hpp"
#include "model/frame.hpp"

namespace axtp {

class FrameEncoder {
public:
    Bytes encode(const Frame& frame) {
        ByteWriter writer;
        writer.writeU8(kAxtpStandardMagic0);
        writer.writeU8(kAxtpStandardMagic1);
        writer.writeU8(frame.header.version);
        writer.writeU8(static_cast<std::uint8_t>(frame.header.payloadType));
        writer.writeU16(static_cast<std::uint16_t>(frame.payload.size()));
        writer.writeU8(frame.header.sourceId);
        writer.writeU8(frame.header.destinationId);
        writer.writeU16(frame.header.messageId);
        writer.writeU8(frame.header.frameIndex);
        writer.writeU8(frame.header.frameCount);
        writer.writeBytes(frame.payload);
        const auto crc = crc16CcittFalse(writer.bytes());
        writer.writeU16(crc);
        return writer.takeBytes();
    }
};

}  // namespace axtp
