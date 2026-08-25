#pragma once

#include <cstdint>
#include <stdexcept>

#include "core/protocol/wire/framed_binary/control_tlv_codec.hpp"
#include "core/protocol/wire/websocket_json_rpc/outbound/json_rpc_encoder.hpp"
#include "core/support/io/byte_writer.hpp"
#include "core/protocol/model/message.hpp"
#include "core/protocol/model/payload.hpp"

namespace axtp {

class PayloadEncoder {
public:
    Message encodeControl(const ControlPayload& payload) {
        ByteWriter writer;
        writer.writeU8(static_cast<std::uint8_t>(payload.opcode));
        writer.writeU16(payload.controlId);
        writer.writeU16(static_cast<std::uint16_t>(payload.statusCode));
        if (!payload.body.empty()) {
            writer.writeBytes(payload.body);
        } else if (payload.opcode == ControlOpcode::Open) {
            writer.writeBytes(ControlTlvCodec::encode(payload.tlv, false));
        } else if (payload.opcode == ControlOpcode::Accept) {
            writer.writeBytes(ControlTlvCodec::encode(payload.tlv, true));
        }
        return Message{0, PayloadType::Control, writer.takeBytes()};
    }

    Message encodeRpc(const RpcPayload& payload) {
        if (isJsonBinaryRpcEncoding(payload.encoding) &&
            hasEndpointMetadata(payload.meta.endpoint)) {
            throw std::invalid_argument(
                "JSON_BINARY does not support endpoint metadata");
        }
        ByteWriter writer;
        if (payload.meta.sourceProtocol == SourceProtocol::JsonRpc &&
            payload.encoding == RpcEncoding::Json) {
            writer.writeU8(static_cast<std::uint8_t>(payload.encoding));
            const auto json = _jsonRpcEncoder.encode(payload);
            writer.writeBytes(json);
            return Message{0, PayloadType::Rpc, writer.takeBytes()};
        }

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

private:
    JsonRpcEncoder _jsonRpcEncoder;
};

}  // namespace axtp
