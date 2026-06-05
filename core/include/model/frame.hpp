#pragma once

#include <cstdint>

#include "model/bytes.hpp"
#include "model/protocol_types.hpp"

namespace axtp {

struct FrameHeader {
    std::uint8_t version = kAxtpVersion1;
    PayloadType payloadType = PayloadType::Rpc;
    std::uint16_t payloadLength = 0;
    std::uint8_t sourceId = 0;
    std::uint8_t destinationId = 0;
    std::uint16_t messageId = 0;
    std::uint8_t frameIndex = 0;
    std::uint8_t frameCount = 1;
};

struct Frame {
    FrameHeader header;
    Bytes payload;
    std::uint16_t crc16 = 0;
};

}  // namespace axtp
