#include <cassert>
#include <cstdint>

#include "io/byte_buffer.hpp"
#include "io/byte_reader.hpp"
#include "io/byte_writer.hpp"
#include "model/frame.hpp"
#include "model/message.hpp"
#include "model/payload.hpp"

int main() {
    {
        axtp::ByteWriter writer;
        writer.writeU8(0x12);
        writer.writeU16(0x3456);
        writer.writeU32(0x789ABCDE);
        writer.writeU64(0x1122334455667788ULL);

        const axtp::Bytes expected = {
            0x12,
            0x56,
            0x34,
            0xDE,
            0xBC,
            0x9A,
            0x78,
            0x88,
            0x77,
            0x66,
            0x55,
            0x44,
            0x33,
            0x22,
            0x11,
        };
        assert(writer.bytes() == expected);

        axtp::ByteReader reader(writer.bytes());
        auto u8 = reader.readU8();
        auto u16 = reader.readU16();
        auto u32 = reader.readU32();
        auto u64 = reader.readU64();
        assert(u8.ok() && u8.value == 0x12);
        assert(u16.ok() && u16.value == 0x3456);
        assert(u32.ok() && u32.value == 0x789ABCDE);
        assert(u64.ok() && u64.value == 0x1122334455667788ULL);
        assert(reader.empty());

        const auto offset = reader.offset();
        auto underflow = reader.readU8();
        assert(!underflow.ok());
        assert(underflow.status.code == axtp::ErrorCode::OutOfRange);
        assert(reader.offset() == offset);
    }

    {
        axtp::ByteBuffer buffer;
        const axtp::Bytes first = {0x01, 0x02, 0x03};
        const axtp::Bytes second = {0x04, 0x05};
        buffer.append(first);
        buffer.append(second.data(), second.size());

        assert(buffer.size() == 5);
        auto peeked = buffer.peek(3);
        assert(peeked.ok());
        assert((peeked.value == axtp::Bytes{0x01, 0x02, 0x03}));
        assert(buffer.size() == 5);

        auto offsetPeek = buffer.peek(2, 2);
        assert(offsetPeek.ok());
        assert((offsetPeek.value == axtp::Bytes{0x03, 0x04}));

        auto consumed = buffer.consume(2);
        assert(consumed.ok());
        assert((consumed.value == axtp::Bytes{0x01, 0x02}));
        assert(buffer.size() == 3);
        assert((buffer.bytes() == axtp::Bytes{0x03, 0x04, 0x05}));

        auto failedConsume = buffer.consume(4);
        assert(!failedConsume.ok());
        assert(failedConsume.status.code == axtp::ErrorCode::OutOfRange);
        assert(buffer.size() == 3);

        buffer.clear();
        assert(buffer.empty());
    }

    {
        axtp::Frame frame;
        frame.header.version = axtp::kAxtpVersion1;
        frame.header.payloadType = axtp::PayloadType::Rpc;
        frame.header.sourceId = 1;
        frame.header.destinationId = 2;
        frame.header.messageId = 100;
        frame.header.frameIndex = 0;
        frame.header.frameCount = 1;
        frame.payload = {0xAA};
        frame.header.payloadLength = static_cast<std::uint16_t>(frame.payload.size());
        assert(frame.header.payloadLength == 1);
        assert(axtp::kStandardFrameHeaderSize == 12);

        axtp::Message message;
        message.messageId = frame.header.messageId;
        message.payloadType = frame.header.payloadType;
        message.body = frame.payload;
        assert(message.messageId == 100);
        assert(message.body.size() == 1);

        axtp::ControlPayload control;
        control.opcode = axtp::ControlOpcode::Open;
        control.controlId = 9;
        control.statusCode = axtp::ErrorCode::Success;
        assert(axtp::kControlPayloadHeaderSize == 5);
        assert(control.body.empty());

        axtp::RpcPayload rpc;
        rpc.encoding = axtp::RpcEncoding::Tlv;
        rpc.op = axtp::RpcOp::Request;
        rpc.requestId = 7;
        rpc.methodOrEventId =
            static_cast<std::uint16_t>(axtp::MethodId::AudioGetAlgorithmConfig);
        rpc.statusCode = axtp::ErrorCode::Success;
        rpc.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        rpc.body = {0x10, 0x20};
        assert(axtp::kBinaryRpcHeaderSize == 11);
        assert(rpc.meta.sourceProtocol == axtp::SourceProtocol::AxtpV1);
        assert(rpc.body.size() == 2);

        axtp::StreamPayload stream;
        stream.streamId = 3;
        stream.seqId = 4;
        stream.cursor = 5;
        stream.data = {0x30};
        assert(axtp::kStreamPayloadHeaderSize == 16);
        assert(stream.data.size() == 1);
    }

    return 0;
}
