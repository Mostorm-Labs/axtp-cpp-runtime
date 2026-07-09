#include <cassert>
#include <cstring>
#include <cstdint>
#include <regex>
#include <utility>
#include <vector>

#include "core/runtime/broker/basic_broker.hpp"
#include "core/runtime/core/axtp_core.hpp"
#include "core/protocol/wire/inbound_processor.hpp"
#include "core/protocol/wire/outbound_processor.hpp"
#include "core/support/io/byte_writer_sink.hpp"
#include "core/support/io/crc16.hpp"
#include "core/runtime/endpoint/axtp_endpoint.hpp"

namespace {

struct CapturingByteWriter : axtp::IByteWriter {
    axtp::Bytes bytes;

    void writeBytes(const axtp::Byte* data, std::size_t size) override {
        bytes.insert(bytes.end(), data, data + size);
    }
};

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

axtp::Bytes encodeRpc(axtp::RpcPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendRpcRequest(std::move(payload));
    return writer.bytes;
}

axtp::Bytes encodeControl(axtp::ControlPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendControl(std::move(payload));
    return writer.bytes;
}

axtp::Bytes makeFrame(axtp::PayloadType payloadType,
                      std::uint16_t messageId,
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
    writer.writeU8(0);
    writer.writeU8(1);
    writer.writeBytes(payload);
    writer.writeU16(axtp::crc16CcittFalse(writer.bytes()));
    return writer.takeBytes();
}

axtp::Bytes makeJsonEnvelopePayload(const char* json) {
    axtp::ByteWriter writer;
    writer.writeU8(static_cast<std::uint8_t>(axtp::RpcEncoding::Json));
    const auto* bytes = reinterpret_cast<const axtp::Byte*>(json);
    writer.writeBytes(bytes, std::strlen(json));
    return writer.takeBytes();
}

}  // namespace

int main() {
    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        broker.registerMethod(0x0101, [](const axtp::RpcPayload& request) {
            assert(request.requestId == 100);
            return axtp::Bytes{0x99, 0x88};
        });

        axtp::RpcPayload request;
        request.encoding = axtp::jsonBinaryRpcEncoding();
        request.op = axtp::RpcOp::Request;
        request.requestId = 100;
        request.methodOrEventId = 0x0101;
        request.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        auto requestBytes = encodeRpc(request);
        endpoint.core().byteSink().onBytes(requestBytes.data(), requestBytes.size());
        endpoint.poll();

        auto responseBytes = endpoint.core().tryPopOutboundBytes();
        assert(responseBytes.has_value());
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(responseBytes->data(), responseBytes->size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].op == axtp::RpcOp::RequestResponse);
        assert(sink.rpcs[0].requestId == 100);
        assert(sink.rpcs[0].methodOrEventId == 0x0101);
        assert((sink.rpcs[0].body == axtp::Bytes{0x99, 0x88}));
    }

    {
        axtp::AxtpCore core;
        axtp::ControlPayload open;
        open.opcode = axtp::ControlOpcode::Open;
        open.controlId = 1;
        auto bytes = encodeControl(open);
        core.byteSink().onBytes(bytes.data(), bytes.size());
        assert(core.controlSessionOpen());
        auto responseBytes = core.tryPopOutboundBytes();
        assert(responseBytes.has_value());
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(responseBytes->data(), responseBytes->size());
        assert(sink.controls.size() == 1);
        assert(sink.controls[0].opcode == axtp::ControlOpcode::Accept);
        assert(sink.controls[0].controlId == 1);
        assert(!sink.controls[0].body.empty());
        assert(sink.controls[0].tlv.valid);
        assert(sink.controls[0].tlv.hasSelectedRpcEncoding);
        assert(sink.controls[0].tlv.selectedRpcEncoding ==
               static_cast<std::uint8_t>(axtp::RpcEncoding::Json));

        responseBytes = core.tryPopOutboundBytes();
        assert(responseBytes.has_value());
        CapturingPayloadSink helloSink;
        axtp::InboundProcessor helloInbound(helloSink);
        helloInbound.onBytes(responseBytes->data(), responseBytes->size());
        assert(helloSink.rpcs.size() == 1);
        assert(helloSink.rpcs[0].op == axtp::RpcOp::Hello);

        axtp::ControlPayload ping;
        ping.opcode = axtp::ControlOpcode::Ping;
        ping.controlId = 2;
        bytes = encodeControl(ping);
        core.byteSink().onBytes(bytes.data(), bytes.size());
        responseBytes = core.tryPopOutboundBytes();
        assert(responseBytes.has_value());
        CapturingPayloadSink pingSink;
        axtp::InboundProcessor pingInbound(pingSink);
        pingInbound.onBytes(responseBytes->data(), responseBytes->size());
        assert(pingSink.controls.size() == 1);
        assert(pingSink.controls[0].opcode == axtp::ControlOpcode::Pong);
    }

    {
        axtp::AxtpCore core;
        core.sendControlOpen(9);
        auto openBytes = core.tryPopOutboundBytes();
        assert(openBytes.has_value());

        CapturingPayloadSink openSink;
        axtp::InboundProcessor openInbound(openSink);
        openInbound.onBytes(openBytes->data(), openBytes->size());
        assert(openSink.controls.size() == 1);
        assert(openSink.controls[0].opcode == axtp::ControlOpcode::Open);
        assert(openSink.controls[0].controlId == 9);
        assert(!openSink.controls[0].body.empty());
        assert(openSink.controls[0].tlv.valid);
        assert(!openSink.controls[0].tlv.hasProtocolVersion);
        assert(openSink.controls[0].tlv.hasMaxFrameSize);
        assert(!openSink.controls[0].tlv.hasMtu);
        assert(openSink.controls[0].tlv.hasSupportedPayloadTypes);
        assert(openSink.controls[0].tlv.hasSupportedRpcEncodings);
        assert(openSink.controls[0].tlv.supportedRpcEncodings == 0x09);
        assert(!core.controlSessionOpen());

        {
            axtp::ControlPayload accept;
            accept.opcode = axtp::ControlOpcode::Accept;
            accept.controlId = 9;
            accept.statusCode = axtp::ErrorCode::Success;
            accept.body = {
                0x01, 0x04, 0x12, 0x34, 0x56, 0x78,
                0x02, 0x01, 0x01,
                0x04, 0x02, 0x10, 0x00,
                0x06, 0x02, 0x09, 0xc4,
                0x07, 0x01, 0x07,
                0x1e, 0x01, 0x01,
                0x0a, 0x02, 0x0b, 0xb8,
                0x0b, 0x01, 0x00,
            };
            auto acceptBytes = encodeControl(accept);
            core.byteSink().onBytes(acceptBytes.data(), acceptBytes.size());
            assert(core.controlSessionOpen());

            auto notice = core.tryTakeControlNotice(axtp::ControlOpcode::Accept);
            assert(notice.has_value());
            assert(notice->controlId == 9);
            assert(notice->statusCode == axtp::ErrorCode::Success);
            assert(notice->tlv.valid);
            assert(notice->tlv.hasMaxFrameSize);
            assert(notice->tlv.maxFrameSize == 4096);
            assert(notice->tlv.hasMtu);
            assert(notice->tlv.mtu == 2500);
            assert(notice->tlv.hasHeartbeatIntervalMs);
            assert(notice->tlv.heartbeatIntervalMs == 3000);
        }

        core.sendControlOpen(9);
        (void)core.tryPopOutboundBytes();

        axtp::ControlPayload accept;
        accept.opcode = axtp::ControlOpcode::Accept;
        accept.controlId = 9;
        accept.statusCode = axtp::ErrorCode::Success;
        auto acceptBytes = encodeControl(accept);
        core.byteSink().onBytes(acceptBytes.data(), acceptBytes.size());
        assert(core.controlSessionOpen());

        auto notice = core.tryTakeControlNotice(axtp::ControlOpcode::Accept);
        assert(notice.has_value());
        assert(notice->controlId == 9);
        assert(notice->statusCode == axtp::ErrorCode::Success);
    }

    {
        axtp::AxtpCore core;
        core.sendControlOpen(10);
        (void)core.tryPopOutboundBytes();

        axtp::ControlPayload accept;
        accept.opcode = axtp::ControlOpcode::Accept;
        accept.controlId = 11;
        accept.statusCode = axtp::ErrorCode::Success;
        auto acceptBytes = encodeControl(accept);
        core.byteSink().onBytes(acceptBytes.data(), acceptBytes.size());
        assert(!core.controlSessionOpen());

        auto notice = core.tryTakeControlNotice(axtp::ControlOpcode::Accept);
        assert(notice.has_value());
        assert(notice->controlId == 11);
    }

    {
        axtp::AxtpCore core;
        core.expectRpcResponse(55);

        axtp::RpcPayload response;
        response.encoding = axtp::jsonBinaryRpcEncoding();
        response.op = axtp::RpcOp::RequestResponse;
        response.requestId = 55;
        response.methodOrEventId = 0x0101;
        response.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        response.body = {0x01};

        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        outbound.sendRpcResponse(response);
        core.byteSink().onBytes(writer.bytes.data(), writer.bytes.size());
        auto matched = core.tryTakeRpcResponse(55);
        assert(matched.has_value());
        assert((matched->body == axtp::Bytes{0x01}));
    }

    {
        axtp::AxtpCore core;
        core.expectRpcResponse(55);
        const auto bytes = makeFrame(
            axtp::PayloadType::Rpc,
            20,
            makeJsonEnvelopePayload(
                R"({"d":{"id":0,"status":{"code":51,"msg":"RPC_PAYLOAD_INVALID","ok":false}},"op":8,"sid":"12345678"})"));
        core.byteSink().onBytes(bytes.data(), bytes.size());
        assert(!core.tryTakeRpcResponse(55).has_value());
        auto any = core.tryTakeAnyRpcResponse();
        assert(any.has_value());
        assert(any->op == axtp::RpcOp::RequestResponse);
        assert(any->requestId == 0);
        assert(any->statusCode == axtp::ErrorCode::RpcPayloadInvalid);
    }

    {
        axtp::AxtpCore core;
        const auto bytes = makeFrame(axtp::PayloadType::Rpc,
                                     21,
                                     makeJsonEnvelopePayload(
                                         R"({"sid":"","op":2,"d":{"eventMasks":""}})"));
        core.byteSink().onBytes(bytes.data(), bytes.size());
        auto identify = core.tryTakeSessionRpc(axtp::RpcOp::Identify);
        assert(identify.has_value());
        assert(!identify->meta.hasRandomSeed);
        auto identifiedBytes = core.tryPopOutboundBytes();
        assert(identifiedBytes.has_value());

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(identifiedBytes->data(), identifiedBytes->size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].op == axtp::RpcOp::Identified);
        assert(std::regex_match(sink.rpcs[0].meta.jsonSid, std::regex("^[0-9A-F]{8}$")));
        assert(sink.rpcs[0].meta.jsonSid != "00000000");
    }

    {
        axtp::AxtpCore core;
        const auto bytes = makeFrame(axtp::PayloadType::Rpc,
                                     22,
                                     makeJsonEnvelopePayload(
                                         R"({"sid":"","op":2,"d":{"randomSeed":305419896}})"));
        core.byteSink().onBytes(bytes.data(), bytes.size());
        auto identifiedBytes = core.tryPopOutboundBytes();
        assert(identifiedBytes.has_value());

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(identifiedBytes->data(), identifiedBytes->size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].op == axtp::RpcOp::Identified);
        assert(std::regex_match(sink.rpcs[0].meta.jsonSid, std::regex("^[0-9A-F]{8}$")));
        assert(sink.rpcs[0].meta.jsonSid != "00000000");
        assert(sink.rpcs[0].meta.jsonSid != "12345678");
    }

    return 0;
}
