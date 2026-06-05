#pragma once

#include <cstdint>

#include "io/byte_writer.hpp"
#include "model/message.hpp"
#include "model/payload.hpp"

namespace axtp {

class PayloadEncoder {
public:
    Message encodeControl(const ControlPayload& payload) {
        ByteWriter writer;
        writer.writeU8(static_cast<std::uint8_t>(payload.opcode));
        writer.writeU16(payload.controlId);
        writer.writeU16(static_cast<std::uint16_t>(payload.statusCode));
        writer.writeBytes(payload.body);
        return Message{0, PayloadType::Control, writer.takeBytes()};
    }

    Message encodeRpc(const RpcPayload& payload) {
        ByteWriter writer;
        writer.writeU8(static_cast<std::uint8_t>(payload.encoding));
        writer.writeU8(static_cast<std::uint8_t>(payload.op));
        writer.writeU32(payload.requestId);
        writer.writeU16(static_cast<std::uint16_t>(payload.methodOrEventId));
        writer.writeU16(static_cast<std::uint16_t>(payload.statusCode));
        writer.writeU8(static_cast<std::uint8_t>(payload.bodyEncoding));
        writer.writeBytes(payload.body);
        return Message{0, PayloadType::Rpc, writer.takeBytes()};
    }

    Message encodeStream(const StreamPayload& payload) {
        ByteWriter writer;
        writer.writeU32(payload.streamId);
        writer.writeU32(payload.seqId);
        writer.writeU64(payload.cursor);
        writer.writeBytes(payload.data);
        return Message{0, PayloadType::Stream, writer.takeBytes()};
    }
};

}  // namespace axtp
