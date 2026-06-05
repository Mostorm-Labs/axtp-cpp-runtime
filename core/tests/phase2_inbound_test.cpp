#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/inbound/inbound_processor.hpp"
#include "io/byte_writer.hpp"
#include "io/crc16.hpp"

namespace {

struct CapturingPayloadSink : axtp::IPayloadSink {
    std::vector<axtp::ControlPayload> controls;
    std::vector<axtp::RpcPayload> rpcs;
    std::vector<axtp::StreamPayload> streams;

    void onControl(axtp::ControlPayload payload) override {
        controls.push_back(std::move(payload));
    }

    void onRpc(axtp::RpcPayload payload) override {
        rpcs.push_back(std::move(payload));
    }

    void onStream(axtp::StreamPayload payload) override {
        streams.push_back(std::move(payload));
    }
};

axtp::Bytes makeFrame(axtp::PayloadType payloadType,
                      std::uint16_t messageId,
                      std::uint8_t frameIndex,
                      std::uint8_t frameCount,
                      const axtp::Bytes& payload) {
    axtp::ByteWriter writer;
    writer.writeU8(axtp::kAxtpStandardMagic0);
    writer.writeU8(axtp::kAxtpStandardMagic1);
    writer.writeU8(axtp::kAxtpVersion1);
    writer.writeU8(static_cast<std::uint8_t>(payloadType));
    writer.writeU16(static_cast<std::uint16_t>(payload.size()));
    writer.writeU8(1);
    writer.writeU8(2);
    writer.writeU16(messageId);
    writer.writeU8(frameIndex);
    writer.writeU8(frameCount);
    writer.writeBytes(payload);
    const auto crc = axtp::crc16CcittFalse(writer.bytes());
    writer.writeU16(crc);
    return writer.takeBytes();
}

axtp::Bytes
makeRpcPayload(std::uint32_t requestId, std::uint16_t methodId, const axtp::Bytes& body) {
    axtp::ByteWriter writer;
    writer.writeU8(static_cast<std::uint8_t>(axtp::RpcEncoding::Tlv));
    writer.writeU8(static_cast<std::uint8_t>(axtp::RpcOp::Request));
    writer.writeU32(requestId);
    writer.writeU16(methodId);
    writer.writeU16(static_cast<std::uint16_t>(axtp::ErrorCode::Success));
    writer.writeU8(static_cast<std::uint8_t>(axtp::RpcBodyEncoding::Tlv8));
    writer.writeBytes(body);
    return writer.takeBytes();
}

}  // namespace

int main() {
    {
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        const auto frame =
            makeFrame(axtp::PayloadType::Rpc, 1, 0, 1, makeRpcPayload(7, 0x0101, {0xAA}));
        inbound.onBytes(frame.data(), 6);
        assert(sink.rpcs.empty());
        inbound.onBytes(frame.data() + 6, frame.size() - 6);
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].op == axtp::RpcOp::Request);
        assert(sink.rpcs[0].encoding == axtp::RpcEncoding::Tlv);
        assert(sink.rpcs[0].requestId == 7);
        assert(sink.rpcs[0].methodOrEventId == 0x0101);
        assert((sink.rpcs[0].body == axtp::Bytes{0xAA}));
    }

    {
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        auto first = makeFrame(axtp::PayloadType::Rpc, 2, 0, 1, makeRpcPayload(8, 0x0101, {0x01}));
        auto second = makeFrame(axtp::PayloadType::Rpc, 3, 0, 1, makeRpcPayload(9, 0x0101, {0x02}));
        first.insert(first.end(), second.begin(), second.end());
        inbound.onBytes(first.data(), first.size());
        assert(sink.rpcs.size() == 2);
        assert(sink.rpcs[0].requestId == 8);
        assert(sink.rpcs[1].requestId == 9);
    }

    {
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        const auto rpc = makeRpcPayload(10, 0x0101, {0x10, 0x11, 0x12, 0x13});
        const axtp::Bytes firstPart(rpc.begin(), rpc.begin() + 7);
        const axtp::Bytes secondPart(rpc.begin() + 7, rpc.end());
        const auto first = makeFrame(axtp::PayloadType::Rpc, 4, 0, 2, firstPart);
        const auto second = makeFrame(axtp::PayloadType::Rpc, 4, 1, 2, secondPart);
        inbound.onBytes(second.data(), second.size());
        assert(sink.rpcs.empty());
        inbound.onBytes(first.data(), first.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].requestId == 10);
        assert((sink.rpcs[0].body == axtp::Bytes{0x10, 0x11, 0x12, 0x13}));
    }

    {
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        axtp::Bytes noise = {0x00, 0x41, 0x00, 0x99};
        const auto frame =
            makeFrame(axtp::PayloadType::Rpc, 5, 0, 1, makeRpcPayload(11, 0x0101, {}));
        noise.insert(noise.end(), frame.begin(), frame.end());
        inbound.onBytes(noise.data(), noise.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].requestId == 11);
    }

    return 0;
}
