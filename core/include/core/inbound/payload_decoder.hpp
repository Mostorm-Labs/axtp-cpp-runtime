#pragma once

#include <cstdint>
#include <utility>

#include "core/inbound/message_reassembler.hpp"
#include "io/byte_reader.hpp"
#include "model/payload.hpp"

namespace axtp {

class IPayloadSink {
public:
    virtual ~IPayloadSink() = default;
    virtual void onControl(ControlPayload payload) = 0;
    virtual void onRpc(RpcPayload payload) = 0;
    virtual void onStream(StreamPayload payload) = 0;
};

class PayloadDecoder : public IMessageSink {
public:
    explicit PayloadDecoder(IPayloadSink& next)
        : _next(next) {}

    void onMessage(Message message) override {
        switch (message.payloadType) {
        case PayloadType::Control:
            decodeControl(std::move(message));
            break;
        case PayloadType::Rpc:
            decodeRpc(std::move(message));
            break;
        case PayloadType::Stream:
            decodeStream(std::move(message));
            break;
        }
    }

private:
    void decodeControl(Message message) {
        if (message.body.size() < kControlPayloadHeaderSize) {
            return;
        }
        ByteReader reader(message.body);
        auto opcode = reader.readU8();
        auto controlId = reader.readU16();
        auto statusCode = reader.readU16();
        auto body = reader.readBytes(reader.remaining());
        if (!opcode.ok() || !controlId.ok() || !statusCode.ok() || !body.ok()) {
            return;
        }

        ControlPayload payload;
        payload.opcode = static_cast<ControlOpcode>(opcode.value);
        payload.controlId = controlId.value;
        payload.statusCode = static_cast<ErrorCode>(statusCode.value);
        payload.body = std::move(body.value);
        _next.onControl(std::move(payload));
    }

    void decodeRpc(Message message) {
        if (message.body.size() < kBinaryRpcHeaderSize) {
            return;
        }
        ByteReader reader(message.body);
        auto encoding = reader.readU8();
        auto op = reader.readU8();
        auto requestId = reader.readU32();
        auto methodOrEventId = reader.readU16();
        auto statusCode = reader.readU16();
        auto bodyEncoding = reader.readU8();
        auto body = reader.readBytes(reader.remaining());
        if (!encoding.ok() || !op.ok() || !requestId.ok() || !methodOrEventId.ok() ||
            !statusCode.ok() || !bodyEncoding.ok() || !body.ok()) {
            return;
        }

        RpcPayload payload;
        payload.encoding = static_cast<RpcEncoding>(encoding.value);
        payload.op = static_cast<RpcOp>(op.value);
        payload.requestId = requestId.value;
        payload.methodOrEventId = methodOrEventId.value;
        payload.statusCode = static_cast<ErrorCode>(statusCode.value);
        payload.bodyEncoding = static_cast<RpcBodyEncoding>(bodyEncoding.value);
        payload.meta.requestId = payload.requestId;
        payload.body = std::move(body.value);
        _next.onRpc(std::move(payload));
    }

    void decodeStream(Message message) {
        if (message.body.size() < kStreamPayloadHeaderSize) {
            return;
        }
        ByteReader reader(message.body);
        auto streamId = reader.readU32();
        auto seqId = reader.readU32();
        auto cursor = reader.readU64();
        auto data = reader.readBytes(reader.remaining());
        if (!streamId.ok() || !seqId.ok() || !cursor.ok() || !data.ok()) {
            return;
        }

        StreamPayload payload;
        payload.streamId = streamId.value;
        payload.seqId = seqId.value;
        payload.cursor = cursor.value;
        payload.data = std::move(data.value);
        _next.onStream(std::move(payload));
    }

    IPayloadSink& _next;
};

}  // namespace axtp
