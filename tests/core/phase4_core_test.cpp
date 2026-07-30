#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "core/runtime/broker/basic_broker.hpp"
#include "core/runtime/core/axtp_core.hpp"
#include "core/protocol/wire/inbound_processor.hpp"
#include "core/protocol/wire/outbound_processor.hpp"
#include "core/support/io/byte_writer_sink.hpp"
#include "core/support/io/crc16.hpp"
#include "core/runtime/endpoint/axtp_endpoint.hpp"
#include "audio_algorithm_config_validator.hpp"

namespace {

void require(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::fprintf(stderr, "%s:%d: requirement failed: %s\n", file, line, expression);
        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __FILE__, __LINE__)

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

axtp::Bytes encodeStream(axtp::StreamPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendStream(std::move(payload));
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
        bool called = false;
        broker.registerRequestValidator(axtp::AudioAlgorithmConfigValidator{});
        broker.registerJsonMethod("audio.setAlgorithmConfig", [&called](const axtp::RpcContext&, std::string_view) {
            called = true;
            return std::string(R"({})");
        });
        axtp::RpcPayload request;
        request.encoding = axtp::RpcEncoding::Json;
        request.op = axtp::RpcOp::Request;
        request.requestId = 3;
        request.methodOrEventId = 0x0902;
        const std::string params = R"({"config":{"noiseSuppression":{"level":999}}})";
        request.body.assign(params.begin(), params.end());
        axtp::BrokerTask task;
        task.type = axtp::BrokerTaskType::RpcRequest;
        task.rpc = request;
        broker.submit(std::move(task));
        broker.poll();
        const auto result = broker.pollResult();
        REQUIRE(result.has_value());
        REQUIRE(result->rpc.statusCode == axtp::ErrorCode::OutOfRange);
        REQUIRE(!called);
    }

    {
        axtp::BasicBroker<> broker;
        axtp::RpcPayload request;
        request.encoding = axtp::RpcEncoding::Json;
        request.op = axtp::RpcOp::Request;
        request.requestId = 39;
        request.methodOrEventId = 0x0902;  // generated audio.setAlgorithmConfig
        const auto response = broker.registry().findMethodName(request.methodOrEventId);
        REQUIRE(response.has_value());

        axtp::BrokerTask task;
        task.type = axtp::BrokerTaskType::RpcRequest;
        task.rpc = request;
        broker.submit(std::move(task));
        broker.poll();
        const auto result = broker.pollResult();
        REQUIRE(result.has_value());
        REQUIRE(result->rpc.statusCode == axtp::ErrorCode::NotSupported);
    }

    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        broker.registerMethod(0x0101, [](const axtp::RpcPayload& request) {
            REQUIRE(request.requestId == 100);
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
        REQUIRE(responseBytes.has_value());
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(responseBytes->data(), responseBytes->size());
        REQUIRE(sink.rpcs.size() == 1);
        REQUIRE(sink.rpcs[0].op == axtp::RpcOp::RequestResponse);
        REQUIRE(sink.rpcs[0].requestId == 100);
        REQUIRE(sink.rpcs[0].methodOrEventId == 0x0101);
        REQUIRE((sink.rpcs[0].body == axtp::Bytes{0x99, 0x88}));
    }

    {
        axtp::AxtpCore core;
        axtp::ControlPayload open;
        open.opcode = axtp::ControlOpcode::Open;
        open.controlId = 1;
        auto bytes = encodeControl(open);
        core.byteSink().onBytes(bytes.data(), bytes.size());
        REQUIRE(core.controlSessionOpen());
        auto responseBytes = core.tryPopOutboundBytes();
        REQUIRE(responseBytes.has_value());
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(responseBytes->data(), responseBytes->size());
        REQUIRE(sink.controls.size() == 1);
        REQUIRE(sink.controls[0].opcode == axtp::ControlOpcode::Accept);
        REQUIRE(sink.controls[0].controlId == 1);
        REQUIRE(!sink.controls[0].body.empty());
        REQUIRE(sink.controls[0].tlv.valid);
        REQUIRE(sink.controls[0].tlv.hasSelectedRpcEncoding);
        REQUIRE(sink.controls[0].tlv.selectedRpcEncoding ==
                static_cast<std::uint8_t>(axtp::RpcEncoding::Json));

        responseBytes = core.tryPopOutboundBytes();
        REQUIRE(responseBytes.has_value());
        CapturingPayloadSink helloSink;
        axtp::InboundProcessor helloInbound(helloSink);
        helloInbound.onBytes(responseBytes->data(), responseBytes->size());
        REQUIRE(helloSink.rpcs.size() == 1);
        REQUIRE(helloSink.rpcs[0].op == axtp::RpcOp::Hello);

        axtp::ControlPayload ping;
        ping.opcode = axtp::ControlOpcode::Ping;
        ping.controlId = 2;
        bytes = encodeControl(ping);
        core.byteSink().onBytes(bytes.data(), bytes.size());
        responseBytes = core.tryPopOutboundBytes();
        REQUIRE(responseBytes.has_value());
        CapturingPayloadSink pingSink;
        axtp::InboundProcessor pingInbound(pingSink);
        pingInbound.onBytes(responseBytes->data(), responseBytes->size());
        REQUIRE(pingSink.controls.size() == 1);
        REQUIRE(pingSink.controls[0].opcode == axtp::ControlOpcode::Pong);
    }

    {
        axtp::AxtpCore core;
        core.sendControlOpen(9);
        auto openBytes = core.tryPopOutboundBytes();
        REQUIRE(openBytes.has_value());

        CapturingPayloadSink openSink;
        axtp::InboundProcessor openInbound(openSink);
        openInbound.onBytes(openBytes->data(), openBytes->size());
        REQUIRE(openSink.controls.size() == 1);
        REQUIRE(openSink.controls[0].opcode == axtp::ControlOpcode::Open);
        REQUIRE(openSink.controls[0].controlId == 9);
        REQUIRE(!openSink.controls[0].body.empty());
        REQUIRE(openSink.controls[0].tlv.valid);
        REQUIRE(!openSink.controls[0].tlv.hasProtocolVersion);
        REQUIRE(openSink.controls[0].tlv.hasMaxFrameSize);
        REQUIRE(!openSink.controls[0].tlv.hasMtu);
        REQUIRE(openSink.controls[0].tlv.hasSupportedPayloadTypes);
        REQUIRE(openSink.controls[0].tlv.hasSupportedRpcEncodings);
        REQUIRE(openSink.controls[0].tlv.supportedRpcEncodings == 0x09);
        REQUIRE(!core.controlSessionOpen());

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
            REQUIRE(core.controlSessionOpen());

            auto notice = core.tryTakeControlNotice(axtp::ControlOpcode::Accept);
            REQUIRE(notice.has_value());
            REQUIRE(notice->controlId == 9);
            REQUIRE(notice->statusCode == axtp::ErrorCode::Success);
            REQUIRE(notice->tlv.valid);
            REQUIRE(notice->tlv.hasMaxFrameSize);
            REQUIRE(notice->tlv.maxFrameSize == 4096);
            REQUIRE(notice->tlv.hasMtu);
            REQUIRE(notice->tlv.mtu == 2500);
            REQUIRE(notice->tlv.hasHeartbeatIntervalMs);
            REQUIRE(notice->tlv.heartbeatIntervalMs == 3000);
        }

        core.sendControlOpen(9);
        (void)core.tryPopOutboundBytes();

        axtp::ControlPayload accept;
        accept.opcode = axtp::ControlOpcode::Accept;
        accept.controlId = 9;
        accept.statusCode = axtp::ErrorCode::Success;
        auto acceptBytes = encodeControl(accept);
        core.byteSink().onBytes(acceptBytes.data(), acceptBytes.size());
        REQUIRE(core.controlSessionOpen());

        auto notice = core.tryTakeControlNotice(axtp::ControlOpcode::Accept);
        REQUIRE(notice.has_value());
        REQUIRE(notice->controlId == 9);
        REQUIRE(notice->statusCode == axtp::ErrorCode::Success);
    }

    {
        // A renegotiation must not retain a heartbeat interval accepted by a
        // previous OPEN.  Otherwise a peer that omits the interval on the
        // replacement session would be incorrectly treated as heartbeat
        // capable by the SDK.
        axtp::ControlSession session;
        auto open = session.makeOpen(21);
        axtp::ControlPayload accept;
        accept.opcode = axtp::ControlOpcode::Accept;
        accept.controlId = open.controlId;
        accept.statusCode = axtp::ErrorCode::Success;
        accept.tlv = axtp::ControlTlvCodec::defaultsForAccept(open.tlv);
        accept.tlv.hasHeartbeatIntervalMs = true;
        accept.tlv.heartbeatIntervalMs = 5000;
        REQUIRE(session.handle(std::move(accept)).has_value() == false);
        REQUIRE(session.negotiatedHeartbeatIntervalMs().has_value());
        REQUIRE(session.negotiatedHeartbeatIntervalMs().value() == 5000);

        auto replacementOpen = session.makeOpen(22);
        REQUIRE(!session.negotiatedHeartbeatIntervalMs().has_value());
        axtp::ControlPayload replacementAccept;
        replacementAccept.opcode = axtp::ControlOpcode::Accept;
        replacementAccept.controlId = replacementOpen.controlId;
        replacementAccept.statusCode = axtp::ErrorCode::Success;
        replacementAccept.tlv = axtp::ControlTlvOptions{};
        replacementAccept.tlv.valid = true;
        // Deliberately omit heartbeatIntervalMs to model an older peer.
        REQUIRE(session.handle(std::move(replacementAccept)).has_value() == false);
        REQUIRE(!session.negotiatedHeartbeatIntervalMs().has_value());
    }

    {
        // Ingress provenance is stamped before Core defers the stream event
        // into its queue.  This is the boundary that fences an old payload
        // when an embedding replaces its media lease before broker dispatch.
        axtp::AxtpCore core;
        axtp::TransportProfile profile;
        profile.kind = axtp::TransportKind::Hid;
        profile.wireMode = axtp::AxtpWireMode::FramedBinary;
        core.configure(profile);
        constexpr std::uint64_t kIngressToken = 0x1122334455667788ULL;
        core.setIngressTokenProvider([kIngressToken] { return kIngressToken; });

        axtp::StreamPayload stream;
        stream.streamId = 0x1234;
        stream.seqId = 7;
        stream.cursor = 99;
        stream.data = {0xAA, 0xBB};
        const auto bytes = encodeStream(stream);
        core.byteSink().onBytes(bytes.data(), bytes.size());
        const auto event = core.pollEvent();
        REQUIRE(event.has_value());
        REQUIRE(event->type == axtp::CoreEventType::StreamData);
        REQUIRE(event->stream.meta.ingressToken == kIngressToken);
        REQUIRE(event->stream.streamId == stream.streamId);
        REQUIRE(event->stream.seqId == stream.seqId);
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
        REQUIRE(!core.controlSessionOpen());

        auto notice = core.tryTakeControlNotice(axtp::ControlOpcode::Accept);
        REQUIRE(notice.has_value());
        REQUIRE(notice->controlId == 11);
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
        REQUIRE(matched.has_value());
        REQUIRE((matched->body == axtp::Bytes{0x01}));
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
        REQUIRE(!core.tryTakeRpcResponse(55).has_value());
        auto any = core.tryTakeAnyRpcResponse();
        REQUIRE(any.has_value());
        REQUIRE(any->op == axtp::RpcOp::RequestResponse);
        REQUIRE(any->requestId == 0);
        REQUIRE(any->statusCode == axtp::ErrorCode::RpcPayloadInvalid);
    }

    {
        axtp::AxtpCore core;
        axtp::TransportProfile profile;
        profile.kind = axtp::TransportKind::WebSocket;
        profile.wireMode = axtp::AxtpWireMode::WebSocketJsonRpc;
        profile.defaultRpcEncoding = axtp::RpcEncoding::Json;
        profile.messageOriented = true;
        profile.supportsTextMessage = true;
        profile.supportsBinaryMessage = false;
        core.configure(profile);

        // Decoder-generated responses are outbound protocol errors, not
        // unmatched responses from a peer.  They must retain their one-way
        // trip through Core while genuine unknown responses are consumed.
        const std::string unknownMethod =
            R"({"sid":"12345678","op":7,"d":{"id":704,"method":"audio.unknown","params":{}}})";
        core.byteSink().onBytes(
            reinterpret_cast<const axtp::Byte*>(unknownMethod.data()),
            unknownMethod.size());
        auto errorBytes = core.tryPopOutboundBytes();
        REQUIRE(errorBytes.has_value());
        const std::string errorJson(errorBytes->begin(), errorBytes->end());
        REQUIRE(errorJson.find(R"("id":704)") != std::string::npos);
        const auto methodNotFoundCode =
            std::to_string(static_cast<std::uint16_t>(
                axtp::ErrorCode::RpcMethodNotFound));
        REQUIRE(errorJson.find(std::string(R"("code":)") + methodNotFoundCode) !=
                std::string::npos);

        const std::string unsupportedBatch =
            R"({"sid":"12345678","op":9,"d":{"id":705,"requests":[]}})";
        core.byteSink().onBytes(
            reinterpret_cast<const axtp::Byte*>(unsupportedBatch.data()),
            unsupportedBatch.size());
        auto batchErrorBytes = core.tryPopOutboundBytes();
        REQUIRE(batchErrorBytes.has_value());
        const std::string batchErrorJson(
            batchErrorBytes->begin(), batchErrorBytes->end());
        REQUIRE(batchErrorJson.find(R"("id":705)") != std::string::npos);
        REQUIRE(batchErrorJson.find(R"("op":10)") != std::string::npos);
        const auto batchUnsupportedCode =
            std::to_string(static_cast<std::uint16_t>(
                axtp::ErrorCode::RpcBatchUnsupported));
        REQUIRE(batchErrorJson.find(std::string(R"("code":)") + batchUnsupportedCode) !=
                std::string::npos);

        const std::string unknownResponse =
            R"({"sid":"12345678","op":8,"d":{"id":706,"status":{"ok":true,"code":0},"result":{}}})";
        core.byteSink().onBytes(
            reinterpret_cast<const axtp::Byte*>(unknownResponse.data()),
            unknownResponse.size());
        REQUIRE(!core.tryPopOutboundBytes().has_value());
        REQUIRE(!core.tryTakeAnyRpcResponse().has_value());
    }

    {
        axtp::AxtpCore core;
        const auto bytes = makeFrame(axtp::PayloadType::Rpc,
                                     21,
                                     makeJsonEnvelopePayload(
                                         R"({"sid":"","op":2,"d":{"eventMasks":""}})"));
        core.byteSink().onBytes(bytes.data(), bytes.size());
        auto identify = core.tryTakeSessionRpc(axtp::RpcOp::Identify);
        REQUIRE(identify.has_value());
        REQUIRE(!identify->meta.hasRandomSeed);
        auto identifiedBytes = core.tryPopOutboundBytes();
        REQUIRE(identifiedBytes.has_value());

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(identifiedBytes->data(), identifiedBytes->size());
        REQUIRE(sink.rpcs.size() == 1);
        REQUIRE(sink.rpcs[0].op == axtp::RpcOp::Identified);
        REQUIRE(std::regex_match(sink.rpcs[0].meta.jsonSid, std::regex("^[0-9A-F]{8}$")));
        REQUIRE(sink.rpcs[0].meta.jsonSid != "00000000");
    }

    {
        axtp::AxtpCore core;
        const auto bytes = makeFrame(axtp::PayloadType::Rpc,
                                     22,
                                     makeJsonEnvelopePayload(
                                         R"({"sid":"","op":2,"d":{"randomSeed":305419896}})"));
        core.byteSink().onBytes(bytes.data(), bytes.size());
        auto identifiedBytes = core.tryPopOutboundBytes();
        REQUIRE(identifiedBytes.has_value());

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(identifiedBytes->data(), identifiedBytes->size());
        REQUIRE(sink.rpcs.size() == 1);
        REQUIRE(sink.rpcs[0].op == axtp::RpcOp::Identified);
        REQUIRE(std::regex_match(sink.rpcs[0].meta.jsonSid, std::regex("^[0-9A-F]{8}$")));
        REQUIRE(sink.rpcs[0].meta.jsonSid != "00000000");
        REQUIRE(sink.rpcs[0].meta.jsonSid != "12345678");
    }

    return 0;
}
