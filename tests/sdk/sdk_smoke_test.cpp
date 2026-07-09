#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/runtime/endpoint/axtp_endpoint.hpp"
#include "core/runtime/testing/mock_transport.hpp"
#include <axtp_sdk.hpp>

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

axtp::Bytes encodeControl(axtp::ControlPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendControl(std::move(payload));
    return writer.bytes;
}

axtp::Bytes encodeRpc(axtp::RpcPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendRpc(std::move(payload));
    return writer.bytes;
}

axtp::Bytes encodeStream(axtp::StreamPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendStream(std::move(payload));
    return writer.bytes;
}

class ScriptedHandshakeTransport : public axtp::ITransport {
public:
    void bind(axtp::IByteSink& sink) override {
        _sink = &sink;
    }

    void open() override {
        _open = true;
    }

    void close() override {
        _open = false;
    }

    void sendBytes(const axtp::Byte* data, std::size_t size) override {
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(data, size);

        for (const auto& control : sink.controls) {
            if (control.opcode != axtp::ControlOpcode::Open) {
                continue;
            }
            sawOpen = true;
            axtp::ControlPayload accept;
            accept.opcode = axtp::ControlOpcode::Accept;
            accept.controlId = control.controlId;
            accept.statusCode = axtp::ErrorCode::Success;
            inject(encodeControl(accept));
            inject(encodeRpc(axtp::JsonRpcEncoder::makeHello()));
        }

        for (const auto& rpc : sink.rpcs) {
            if (rpc.op == axtp::RpcOp::Identify) {
                sawIdentify = true;
                assert(rpc.meta.jsonSid.empty());
                const std::string body(rpc.body.begin(), rpc.body.end());
                assert(body.find("rpcVersion") == std::string::npos);
                assert(body.find(R"("randomSeed":305419896)") != std::string::npos);
                inject(encodeRpc(axtp::JsonRpcEncoder::makeIdentified("12345678")));
                continue;
            }
            if (rpc.op == axtp::RpcOp::Request) {
                sawBusinessRequest = true;
                assert(rpc.meta.jsonSid == "12345678");
                axtp::RpcPayload response;
                response.encoding = axtp::RpcEncoding::Json;
                response.op = axtp::RpcOp::RequestResponse;
                response.requestId = rpc.requestId;
                response.methodOrEventId = rpc.methodOrEventId;
                response.statusCode = axtp::ErrorCode::Success;
                response.bodyEncoding = axtp::RpcBodyEncoding::None;
                response.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
                response.meta.jsonSid = rpc.meta.jsonSid;
                response.body = {'{', '}'};
                inject(encodeRpc(response));
            }
        }
    }

    axtp::TransportProfile profile() const override {
        return axtp::TransportProfile{
            axtp::TransportKind::Mock,
            axtp::AxtpWireMode::FramedBinary,
            axtp::RpcEncoding::Json,
            false,
            false,
            true,
            4096,
        };
    }

    bool isOpen() const {
        return _open;
    }

    bool sawOpen = false;
    bool sawIdentify = false;
    bool sawBusinessRequest = false;

private:
    void inject(const axtp::Bytes& bytes) {
        if (_sink != nullptr) {
            _sink->onBytes(bytes.data(), bytes.size());
        }
    }

    axtp::IByteSink* _sink = nullptr;
    bool _open = false;
};

}  // namespace

int main() {
    axtp::sdk::AxtpClient client;
    client.attachTransport(std::make_unique<axtp::MockTransport>());
    assert(client.isConnected());

    client.registerMethod(
        static_cast<std::uint16_t>(axtp::MethodId::AudioGetAlgorithmConfig),
        [](const axtp::RpcPayload&) {
            const std::string body = R"({"noiseSuppression":{"enabled":true,"level":3}})";
            return axtp::Bytes(body.begin(), body.end());
        });
    client.registerMethod(static_cast<std::uint16_t>(axtp::MethodId::AudioSetAlgorithmConfig),
                          [](const axtp::RpcPayload& request) {
                              if (request.encoding == axtp::RpcEncoding::Json) {
                                  const std::string body(request.body.begin(), request.body.end());
                                  assert(body == "{}" ||
                                         body.find("noiseSuppression") != std::string::npos);
                              } else {
                                  assert((request.body == axtp::Bytes{0x01, 0x01, 0x50}));
                              }
                              return axtp::Bytes{};
                          });
    client.registerMethod(static_cast<std::uint16_t>(axtp::MethodId::AudioGetAlgorithmCapabilities),
                          [](const axtp::RpcPayload&) {
                              const std::string body =
                                  R"({"algorithms":{"noiseSuppression":{"level":{"min":0,"max":5}}}})";
                              return axtp::Bytes(body.begin(), body.end());
                          });
    client.registerMethod(0x90010001, [](const axtp::RpcPayload& request) { return request.body; });
    client.registry().addMethod(0x90010001, "vendor.echo");

    axtp::RpcPayload raw;
    raw.encoding = axtp::RpcEncoding::Json;
    raw.op = axtp::RpcOp::Request;
    raw.methodOrEventId = static_cast<std::uint16_t>(axtp::MethodId::AudioGetAlgorithmConfig);
    raw.bodyEncoding = axtp::RpcBodyEncoding::None;
    raw.body = {'{', '}'};
    auto response = client.callRaw(raw);
    assert(response.statusCode == axtp::ErrorCode::Success);
    assert(response.op == axtp::RpcOp::RequestResponse);

    const auto dynamicJsonByName = client.callJson("audio.getAlgorithmConfig", "{}");
    assert(dynamicJsonByName.find("noiseSuppression") != std::string::npos);

    const auto dynamicJsonById = client.callJson(0x90010001, R"({"hello":true})");
    assert(dynamicJsonById == R"({"hello":true})");

    auto tlv = client.callTlv("audio.setAlgorithmConfig", axtp::Bytes{0x01, 0x01, 0x50});
    assert((tlv == axtp::Bytes{}));

    auto rawBytes = client.callRawBytes(0x90010001, axtp::Bytes{0xCA, 0xFE});
    assert((rawBytes == axtp::Bytes{0xCA, 0xFE}));

    {
        axtp::sdk::AxtpClient streamClient;
        auto transport = std::make_unique<axtp::MockTransport>();
        auto* transportPtr = transport.get();
        streamClient.attachTransport(std::move(transport));

        axtp::StreamPayload stream;
        stream.streamId = 0x1001;
        stream.seqId = 7;
        stream.cursor = 12;
        stream.data = {0xF0, 0x0D};
        streamClient.sendStream(stream);

        const auto outgoing = transportPtr->tryPopOutgoing();
        assert(outgoing.has_value());
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(outgoing->data(), outgoing->size());
        assert(sink.streams.size() == 1);
        assert(sink.streams[0].streamId == 0x1001);
        assert(sink.streams[0].seqId == 7);
        assert(sink.streams[0].cursor == 12);
        assert((sink.streams[0].data == axtp::Bytes{0xF0, 0x0D}));
    }

    axtp::sdk::AxtpDevice device(client);
    auto config = device.audio.getAlgorithmConfig();
    (void)config;

    auto setResponse =
        device.audio.setAlgorithmConfig(axtp::AudioSetAlgorithmConfigRequest{});
    (void)setResponse;
    auto capabilities = client.callTyped<axtp::MethodId::AudioGetAlgorithmCapabilities>(
        axtp::AudioGetAlgorithmCapabilitiesRequest{});
    (void)capabilities;

    const auto methods = device.capability.methods();
    assert(!methods.empty());
    assert(axtp::RegistryLookup::methodIdByName("audio.getAlgorithmConfig").has_value());

    client.close();
    assert(!client.isConnected());

    {
        axtp::sdk::AxtpClient handshakeClient;
        auto transport = std::make_unique<ScriptedHandshakeTransport>();
        auto* transportPtr = transport.get();
        handshakeClient.attachTransport(std::move(transport));
        assert(transportPtr->isOpen());

        axtp::sdk::AppReadyOptions options;
        options.randomSeed = 0x12345678;
        std::vector<std::string> appReadyTrace;
        options.trace = [&appReadyTrace](const axtp::sdk::AppReadyTraceEvent& event) {
            if (event.stage == "control-open" || event.stage == "control-accept" ||
                event.stage == "framing-ready") {
                assert(!event.hasRandomSeed);
            }
            if (event.stage == "identify") {
                assert(event.hasRandomSeed);
                assert(event.randomSeed == 0x12345678);
            }
            appReadyTrace.push_back(event.stage + ":" + event.action);
        };
        const auto appReady = handshakeClient.ensureAppReady(options);
        assert(appReady.ok);
        assert(appReady.sid == "12345678");
        assert(appReady.hasRandomSeed);
        assert(appReady.randomSeed == 0x12345678);
        assert(handshakeClient.isAppReady());
        assert(handshakeClient.sessionSid() == "12345678");
        assert(transportPtr->sawOpen);
        assert(transportPtr->sawIdentify);
        assert(!appReadyTrace.empty());
        assert(std::find(appReadyTrace.begin(),
                         appReadyTrace.end(),
                         "control-open:send") != appReadyTrace.end());
        assert(std::find(appReadyTrace.begin(), appReadyTrace.end(), "hello:receive") !=
               appReadyTrace.end());
        assert(std::find(appReadyTrace.begin(), appReadyTrace.end(), "identify:send") !=
               appReadyTrace.end());
        assert(std::find(appReadyTrace.begin(), appReadyTrace.end(), "identified:receive") !=
               appReadyTrace.end());
        assert(std::find(appReadyTrace.begin(), appReadyTrace.end(), "app-ready:ready") !=
               appReadyTrace.end());

        axtp::RpcPayload request;
        request.encoding = axtp::RpcEncoding::Json;
        request.op = axtp::RpcOp::Request;
        request.methodOrEventId =
            static_cast<std::uint16_t>(axtp::MethodId::AudioGetAlgorithmConfig);
        request.bodyEncoding = axtp::RpcBodyEncoding::None;
        request.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
        request.meta.jsonMethodOrEventName = "audio.getAlgorithmConfig";
        request.body = {'{', '}'};

        axtp::sdk::CallOptions callOptions;
        callOptions.acceptAnyResponse = true;
        auto response = handshakeClient.callRaw(std::move(request), callOptions);
        assert(response.statusCode == axtp::ErrorCode::Success);
        assert(response.requestId == 1);
        assert(transportPtr->sawBusinessRequest);
    }

    axtp::sdk::AxtpClient tcpConnectorClient;
    tcpConnectorClient.connect(axtp::sdk::TcpEndpoint{"127.0.0.1", 1});
    assert(!tcpConnectorClient.isConnected());
    assert(tcpConnectorClient.lastError().code == axtp::ErrorCode::NotSupported);

    {
        axtp::sdk::AxtpClient client;
        std::vector<axtp::StreamPayload> received;
        client.setStreamHandler(
            [&received](const axtp::BrokerContext&, const axtp::StreamPayload& stream) {
                received.push_back(stream);
            });

        auto transport = std::make_unique<axtp::MockTransport>();
        auto* rawTransport = transport.get();
        client.attachTransport(std::move(transport));

        axtp::StreamPayload stream;
        stream.streamId = 0x1001;
        stream.seqId = 7;
        stream.cursor = 123456;
        stream.data = {0x00, 0x00, 0x01, 0x65};
        rawTransport->injectIncoming(encodeStream(stream));

        client.poll();

        assert(received.size() == 1);
        assert(received.front().streamId == 0x1001);
        assert(received.front().seqId == 7);
        assert(received.front().cursor == 123456);
        assert(received.front().data.size() == 4);
    }
}
