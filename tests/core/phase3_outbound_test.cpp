#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/protocol/wire/inbound_processor.hpp"
#include "core/protocol/wire/outbound_processor.hpp"
#include "core/protocol/wire/websocket_json_rpc/inbound/json_rpc_payload_decoder.hpp"
#include "core/protocol/generated/axtp_generated_version.hpp"
#include "json_rpc/rpc_client_session.hpp"
#include "core/support/io/byte_writer_sink.hpp"

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

}  // namespace

int main() {
    for (const auto& version : std::vector<std::optional<std::string>>{
             "1.0.0", "1.1.0", "1.0.1", "2.0.0", "not-semver", std::nullopt}) {
        nlohmann::json hello = {{"sid", ""}, {"op", 0}, {"d", nlohmann::json::object()}};
        if (version) hello["d"]["axtpVersion"] = *version;
        axtp::RpcClientSession session;
        const auto identify = session.acceptHello(hello, 7);
        assert(identify.at("op") == static_cast<int>(axtp::RpcOp::Identify));
        assert(session.observedAxtpVersion() == version);
    }
    {
        axtp::RpcClientSession session;
        bool rejected = false;
        try { (void)session.acceptHello({{"sid", ""}, {"op", 7}, {"d", nlohmann::json::object()}}, 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }

    for (const std::string d : {
             R"({"axtpVersion":"1.0.0"})",
             R"({"axtpVersion":"1.1.0"})",
             R"({"axtpVersion":"1.0.1"})",
             R"({"axtpVersion":"2.0.0"})",
             R"({"axtpVersion":"not-semver"})",
             R"({})",
         }) {
        const std::string hello = R"({"sid":"","op":0,"d":)" + d + "}";
        CapturingPayloadSink sink;
        axtp::JsonRpcPayloadDecoder::decode(
            reinterpret_cast<const axtp::Byte*>(hello.data()),
            hello.size(),
            sink,
            axtp::SourceProtocol::JsonRpc);
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].op == axtp::RpcOp::Hello);
        assert(nlohmann::json::parse(sink.rpcs[0].body) == nlohmann::json::parse(d));
    }

    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        axtp::RpcPayload rpc;
        rpc.encoding = axtp::jsonBinaryRpcEncoding();
        rpc.op = axtp::RpcOp::Request;
        rpc.requestId = 42;
        rpc.methodOrEventId = 0x0101;
        rpc.statusCode = axtp::ErrorCode::Success;
        rpc.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        rpc.body = {0x01, 0x02, 0x03};

        const auto message = axtp::PayloadEncoder{}.encodeRpc(rpc);
        assert((message.body == axtp::Bytes{
                                    0x04,
                                    0x07,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x2A,
                                    0x01,
                                    0x01,
                                    0x00,
                                    0x00,
                                    0x01,
                                    0x01,
                                    0x02,
                                    0x03,
                                }));
        outbound.sendRpcRequest(rpc);

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].requestId == 42);
        assert(sink.rpcs[0].methodOrEventId == 0x0101);
        assert((sink.rpcs[0].body == axtp::Bytes{0x01, 0x02, 0x03}));
    }

    {
        axtp::RpcPayload rpc;
        rpc.encoding = axtp::jsonBinaryRpcEncoding();
        rpc.op = axtp::RpcOp::Request;
        rpc.requestId = 47;
        rpc.methodOrEventId = 0x0901;
        rpc.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        rpc.meta.endpoint.src = "ep_app";
        rpc.meta.endpoint.dst = "ep_device";
        bool rejected = false;
        try {
            (void)axtp::PayloadEncoder{}.encodeRpc(rpc);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }

    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer, 24);
        axtp::RpcPayload rpc;
        rpc.encoding = axtp::jsonBinaryRpcEncoding();
        rpc.op = axtp::RpcOp::Request;
        rpc.requestId = 43;
        rpc.methodOrEventId = 0x0101;
        rpc.statusCode = axtp::ErrorCode::Success;
        rpc.bodyEncoding = axtp::RpcBodyEncoding::None;
        for (std::uint8_t value = 0; value < 40; ++value) {
            rpc.body.push_back(value);
        }
        outbound.sendRpcRequest(rpc);

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].requestId == 43);
        assert(sink.rpcs[0].body == rpc.body);
    }

    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        axtp::RpcPayload rpc;
        rpc.encoding = axtp::RpcEncoding::Json;
        rpc.op = axtp::RpcOp::Request;
        rpc.requestId = 44;
        rpc.methodOrEventId = 0x0901;
        rpc.bodyEncoding = axtp::RpcBodyEncoding::None;
        rpc.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
        rpc.meta.jsonSid = "1234abcd";
        rpc.meta.jsonMethodOrEventName = "audio.getAlgorithmConfig";
        const std::string body = "{}";
        rpc.body.assign(body.begin(), body.end());
        outbound.sendRpcRequest(rpc);

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].encoding == axtp::RpcEncoding::Json);
        assert(sink.rpcs[0].op == axtp::RpcOp::Request);
        assert(sink.rpcs[0].requestId == 44);
        assert(sink.rpcs[0].methodOrEventId == 0x0901);
        assert(sink.rpcs[0].meta.jsonSid == "1234abcd");
        assert((sink.rpcs[0].body == axtp::Bytes{'{', '}'}));
    }

    {
        axtp::RpcPayload legacy;
        legacy.encoding = axtp::RpcEncoding::Json;
        legacy.op = axtp::RpcOp::Request;
        legacy.requestId = 41;
        legacy.methodOrEventId = 0x0901;
        legacy.bodyEncoding = axtp::RpcBodyEncoding::None;
        legacy.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
        legacy.meta.jsonSid = "12345678";
        legacy.meta.jsonMethodOrEventName = "audio.getAlgorithmConfig";
        legacy.body = {'{', '}'};

        const auto bytes = axtp::JsonRpcEncoder{}.encode(legacy);
        const std::string text(bytes.begin(), bytes.end());
        assert(text ==
               R"({"d":{"id":41,"method":"audio.getAlgorithmConfig","params":{}},"op":7,"sid":"12345678"})");
    }

    {
        axtp::RpcPayload request;
        request.encoding = axtp::RpcEncoding::Json;
        request.op = axtp::RpcOp::Request;
        request.requestId = 45;
        request.methodOrEventId = 0x0901;
        request.meta.jsonSid = "12345678";
        request.meta.jsonMethodOrEventName = "audio.getAlgorithmConfig";
        request.meta.endpoint.src = "ep_app";
        request.meta.endpoint.dst = "ep_device";

        auto object = nlohmann::json::parse(axtp::JsonRpcEncoder{}.encode(request));
        assert(object.at("m").at("src") == "ep_app");
        assert(object.at("m").at("dst") == "ep_device");

        axtp::RpcPayload response;
        response.encoding = axtp::RpcEncoding::Json;
        response.op = axtp::RpcOp::RequestResponse;
        response.requestId = 45;
        response.meta.jsonSid = "12345678";
        response.meta.endpoint.src = "ep_device";
        response.meta.endpoint.dst = "ep_app";
        object = nlohmann::json::parse(axtp::JsonRpcEncoder{}.encode(response));
        assert(object.at("m").at("src") == "ep_device");
        assert(object.at("m").at("dst") == "ep_app");

        axtp::RpcPayload event;
        event.encoding = axtp::RpcEncoding::Json;
        event.op = axtp::RpcOp::Event;
        event.methodOrEventId = 0x0901;
        event.meta.jsonSid = "12345678";
        event.meta.jsonMethodOrEventName = "audio.algorithmConfigChanged";
        event.meta.endpoint.src = "ep_device";
        object = nlohmann::json::parse(axtp::JsonRpcEncoder{}.encode(event));
        assert(object.at("m").at("src") == "ep_device");
        assert(!object.at("m").contains("dst"));

        event.meta.endpoint.dst = "ep_app";
        object = nlohmann::json::parse(axtp::JsonRpcEncoder{}.encode(event));
        assert(object.at("m").at("src") == "ep_device");
        assert(object.at("m").at("dst") == "ep_app");
    }

    {
        axtp::RpcPayload request;
        request.encoding = axtp::RpcEncoding::Json;
        request.op = axtp::RpcOp::Request;
        request.requestId = 46;
        request.meta.endpoint.src = "";
        bool rejected = false;
        try {
            (void)axtp::JsonRpcEncoder{}.encode(request);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }

    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        outbound.sendRpc(axtp::JsonRpcEncoder::makeHello());

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].op == axtp::RpcOp::Hello);
        const std::string body(sink.rpcs[0].body.begin(), sink.rpcs[0].body.end());
        assert(body.find("axtpVersion") != std::string::npos);
        assert(body.find("rpcVersion") == std::string::npos);
        assert(nlohmann::json::parse(body).at("axtpVersion") == axtp::generated::kSpecVersion);
    }

    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        axtp::RpcPayload rpc =
            axtp::JsonRpcEncoder::makeIdentify(0x12345678, "");
        outbound.sendRpc(rpc);

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].encoding == axtp::RpcEncoding::Json);
        assert(sink.rpcs[0].op == axtp::RpcOp::Identify);
        assert(sink.rpcs[0].meta.jsonSid.empty());
        const std::string body(sink.rpcs[0].body.begin(), sink.rpcs[0].body.end());
        assert(body.find("rpcVersion") == std::string::npos);
        assert(body.find(R"("eventMasks":"")") != std::string::npos);
        assert(body.find(R"("randomSeed":305419896)") != std::string::npos);
    }

    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        outbound.sendRpc(axtp::JsonRpcEncoder::makeIdentified("1234abcd"));

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].op == axtp::RpcOp::Identified);
        assert(sink.rpcs[0].meta.jsonSid == "1234abcd");
        const std::string body(sink.rpcs[0].body.begin(), sink.rpcs[0].body.end());
        assert(body == "{}");
        assert(body.find("negotiatedRpcVersion") == std::string::npos);
    }

    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        axtp::ControlPayload control;
        control.opcode = axtp::ControlOpcode::Open;
        control.controlId = 7;
        control.statusCode = axtp::ErrorCode::Success;
        control.body = {0x10};
        outbound.sendControl(control);

        axtp::StreamPayload stream;
        stream.streamId = 9;
        stream.seqId = 10;
        stream.cursor = 11;
        stream.data = {0x20, 0x21};
        outbound.sendStream(stream);

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.controls.size() == 1);
        assert(sink.controls[0].opcode == axtp::ControlOpcode::Open);
        assert(sink.controls[0].controlId == 7);
        assert((sink.controls[0].body == axtp::Bytes{0x10}));
        assert(sink.streams.size() == 1);
        assert(sink.streams[0].streamId == 9);
        assert(sink.streams[0].seqId == 10);
        assert(sink.streams[0].cursor == 11);
        assert((sink.streams[0].data == axtp::Bytes{0x20, 0x21}));
    }

    return 0;
}
