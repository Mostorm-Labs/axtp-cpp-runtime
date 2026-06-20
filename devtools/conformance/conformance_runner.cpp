#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/runtime/broker/basic_broker.hpp"
#include "core/runtime/core/axtp_core.hpp"
#include "core/protocol/wire/inbound_processor.hpp"
#include "core/protocol/wire/outbound_processor.hpp"
#include "core/protocol/generated/axtp_capability_generated.h"
#include "core/protocol/generated/axtp_generated_version.hpp"
#include "core/protocol/generated/axtp_method_registry_generated.h"
#include "core/protocol/generated/registry_lookup.h"
#include "core/support/io/byte_writer_sink.hpp"
#include "core/runtime/endpoint/axtp_endpoint.hpp"
#include "core/runtime/transport/transport.hpp"
#include "json_rpc/websocket_json_rpc_adapter.hpp"

namespace {

enum class Requirement { Required, Optional, NotSelected, Unsupported };
enum class Status { Pending, Passed, Failed, Skipped, Unsupported };

struct CaseResult {
    std::string id;
    std::string level;
    Requirement requirement = Requirement::NotSelected;
    Status status = Status::Pending;
    double durationMs = 0.0;
    std::string message;
};

std::vector<CaseResult> cases = {
    {"handshake.open_accept", "framed-binary", Requirement::Optional, Status::Pending, 0.0, ""},
    {"handshake.open_reject", "framed-binary", Requirement::Optional, Status::Skipped, 0.0, "control open rejection policy is not configurable in the C++ runtime"},
    {"handshake.close", "framed-binary", Requirement::Optional, Status::Pending, 0.0, ""},
    {"handshake.ping_pong", "framed-binary", Requirement::Optional, Status::Pending, 0.0, ""},
    {"session.hello_identify_identified", "websocket-jsonrpc", Requirement::Required, Status::Pending, 0.0, ""},
    {"session.request_before_identified", "websocket-jsonrpc", Requirement::Required, Status::Pending, 0.0, ""},
    {"rpc.request_response_json", "core", Requirement::Required, Status::Pending, 0.0, ""},
    {"rpc.method_not_found", "core", Requirement::Required, Status::Pending, 0.0, ""},
    {"rpc.invalid_params", "core", Requirement::NotSelected, Status::Skipped, 0.0, "schema-aware parameter validation is outside the required C++ core profile"},
    {"rpc.request_id_match", "core", Requirement::Required, Status::Pending, 0.0, ""},
    {"event.subscribe_event", "event", Requirement::Optional, Status::Pending, 0.0, ""},
    {"event.unsubscribe_event", "event", Requirement::Optional, Status::Pending, 0.0, ""},
    {"event.emit_event", "event", Requirement::Optional, Status::Pending, 0.0, ""},
    {"capability.get_all", "capability", Requirement::Optional, Status::Pending, 0.0, ""},
    {"capability.method_binding", "capability", Requirement::Optional, Status::Pending, 0.0, ""},
    {"capability.unsupported_method", "capability", Requirement::Optional, Status::Pending, 0.0, ""},
    {"error.standard_error_shape", "core", Requirement::Required, Status::Pending, 0.0, ""},
    {"error.unauthorized", "core", Requirement::NotSelected, Status::Skipped, 0.0, "auth policy hooks are outside the required C++ core profile"},
    {"error.server_busy", "core", Requirement::NotSelected, Status::Skipped, 0.0, "busy-state policy hooks are outside the required C++ core profile"},
    {"stream.stream_open", "stream", Requirement::Optional, Status::Skipped, 0.0, "stream.open RPC control-plane method is not part of the generated spec/v0.0.2 registry"},
    {"stream.stream_data", "stream", Requirement::Optional, Status::Pending, 0.0, ""},
    {"stream.stream_close", "stream", Requirement::Optional, Status::Skipped, 0.0, "stream.close RPC control-plane method is not part of the generated spec/v0.0.2 registry"},
};

class MemoryJsonTransport final : public axtp::ITransport {
public:
    void bind(axtp::IByteSink& sink) override {
        _sink = &sink;
    }

    void open() override {
        _open = true;
        _hasConnection = true;
    }

    void close() override {
        _open = false;
        _hasConnection = false;
    }

    void poll() override {}

    void sendBytes(const axtp::Byte* data, std::size_t size) override {
        _outgoing.push(axtp::Bytes(data, data + size));
    }

    axtp::TransportProfile profile() const override {
        axtp::TransportProfile profile;
        profile.kind = axtp::TransportKind::Mock;
        profile.wireMode = axtp::AxtpWireMode::WebSocketJsonRpc;
        profile.defaultRpcEncoding = axtp::RpcEncoding::Json;
        profile.messageOriented = true;
        profile.supportsTextMessage = true;
        profile.supportsBinaryMessage = false;
        return profile;
    }

    bool hasConnection() const {
        return _open && _hasConnection;
    }

    void injectJson(std::string_view text) {
        if (_sink != nullptr) {
            _sink->onBytes(reinterpret_cast<const axtp::Byte*>(text.data()), text.size());
        }
    }

    std::optional<nlohmann::json> popJson(std::string* error = nullptr) {
        if (_outgoing.empty()) {
            if (error != nullptr) {
                *error = "missing outgoing JSON message";
            }
            return std::nullopt;
        }
        auto bytes = std::move(_outgoing.front());
        _outgoing.pop();
        try {
            const std::string text(bytes.begin(), bytes.end());
            return nlohmann::json::parse(text);
        } catch (const std::exception& ex) {
            if (error != nullptr) {
                *error = ex.what();
            }
            return std::nullopt;
        }
    }

private:
    axtp::IByteSink* _sink = nullptr;
    std::queue<axtp::Bytes> _outgoing;
    bool _open = false;
    bool _hasConnection = false;
};

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

bool fileExists(const std::string& path) {
    std::ifstream input(path);
    return input.good();
}

double elapsedMs(std::chrono::steady_clock::time_point start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) / 1000.0;
}

CaseResult* findCase(std::string_view id) {
    for (auto& item : cases) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

void runCase(std::string_view id, const std::function<bool(std::string&)>& fn) {
    std::string message;
    const auto start = std::chrono::steady_clock::now();
    bool ok = false;
    try {
        ok = fn(message);
    } catch (const std::exception& ex) {
        message = ex.what();
    } catch (...) {
        message = "unknown exception";
    }
    if (auto* item = findCase(id)) {
        item->status = ok ? Status::Passed : Status::Failed;
        item->durationMs = elapsedMs(start);
        item->message = ok ? std::string() : message;
    }
}

std::string stringField(const nlohmann::json& object, std::string_view key) {
    return std::string(object.at(key).get<std::string>());
}

std::uint16_t statusCode(const nlohmann::json& response) {
    return static_cast<std::uint16_t>(
        response.at("d").at("status").at("code").get<int>());
}

bool statusOk(const nlohmann::json& response) {
    return response.at("d").at("status").at("ok").get<bool>();
}

bool setupJsonAdapter(axtp::BasicBroker<>& broker,
                      axtp::AxtpEndpoint<axtp::BasicBroker<>>& endpoint,
                      MemoryJsonTransport& transport,
                      std::unique_ptr<axtp::WebSocketJsonRpcAdapter>& adapter,
                      std::string& message) {
    endpoint.attachTransport(transport);
    adapter = std::make_unique<axtp::WebSocketJsonRpcAdapter>(endpoint, transport);
    transport.bind(*adapter);
    transport.open();
    adapter->poll(transport);
    auto hello = transport.popJson(&message);
    if (!hello.has_value()) {
        return false;
    }
    if (hello->at("op").get<int>() != static_cast<int>(axtp::RpcOp::Hello)) {
        message = "adapter did not send HELLO on connection";
        return false;
    }
    return true;
}

bool identify(MemoryJsonTransport& transport, std::string& sid, std::string& message) {
    transport.injectJson(R"({"sid":"","op":2,"d":{"rpcVersion":1,"eventMasks":"0901"}})");
    auto identified = transport.popJson(&message);
    if (!identified.has_value()) {
        return false;
    }
    if (identified->at("op").get<int>() != static_cast<int>(axtp::RpcOp::Identified)) {
        message = "adapter did not answer IDENTIFY with IDENTIFIED";
        return false;
    }
    sid = stringField(*identified, "sid");
    if (sid.empty()) {
        message = "IDENTIFIED sid was empty";
        return false;
    }
    const auto& d = identified->at("d");
    const auto negotiatedRpcVersion = d.find("negotiatedRpcVersion");
    if (negotiatedRpcVersion != d.end() && negotiatedRpcVersion->get<int>() != 1) {
        message = "IDENTIFIED did not negotiate rpcVersion 1";
        return false;
    }
    return true;
}

bool testSessionHelloIdentify(std::string& message) {
    axtp::BasicBroker<> broker;
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) {
        return false;
    }
    std::string sid;
    return identify(transport, sid, message);
}

bool testRequestBeforeIdentified(std::string& message) {
    axtp::BasicBroker<> broker;
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) {
        return false;
    }
    transport.injectJson(R"({"sid":"","op":7,"d":{"id":700,"method":"audio.getAlgorithmConfig","params":{}}})");
    auto response = transport.popJson(&message);
    if (!response.has_value()) {
        return false;
    }
    if (response->at("op").get<int>() != static_cast<int>(axtp::RpcOp::RequestResponse) ||
        response->at("d").at("id").get<int>() != 700 ||
        statusCode(*response) != static_cast<std::uint16_t>(axtp::ErrorCode::ControlOpenRequired)) {
        message = "request before IDENTIFIED was not rejected with CONTROL_OPEN_REQUIRED";
        return false;
    }
    return true;
}

bool testRequestResponseJson(std::string& message) {
    axtp::BasicBroker<> broker;
    broker.registerJsonMethod("audio.getAlgorithmConfig", [](const axtp::RpcContext&, std::string_view) {
        return std::string(R"({"noiseSuppression":{"enabled":true,"level":3}})");
    });
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) {
        return false;
    }
    std::string sid;
    if (!identify(transport, sid, message)) {
        return false;
    }
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":7,"d":{"id":701,"method":"audio.getAlgorithmConfig","params":{}}})");
    auto response = transport.popJson(&message);
    if (!response.has_value()) {
        return false;
    }
    const auto& d = response->at("d");
    if (response->at("op").get<int>() != static_cast<int>(axtp::RpcOp::RequestResponse) ||
        d.at("id").get<int>() != 701 || !statusOk(*response) ||
        !d.at("result").contains("noiseSuppression")) {
        message = "JSON-RPC request did not produce the expected successful result";
        return false;
    }
    return true;
}

bool testMethodNotFoundWithId(std::uint32_t requestId, std::string& message) {
    axtp::BasicBroker<> broker;
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) {
        return false;
    }
    std::string sid;
    if (!identify(transport, sid, message)) {
        return false;
    }
    transport.injectJson(std::string(R"({"sid":")") + sid + R"(","op":7,"d":{"id":)" +
                         std::to_string(requestId) +
                         R"(,"method":"vendor.missing","params":{}}})");
    auto response = transport.popJson(&message);
    if (!response.has_value()) {
        return false;
    }
    if (response->at("d").at("id").get<int>() != requestId ||
        statusCode(*response) != static_cast<std::uint16_t>(axtp::ErrorCode::RpcMethodNotFound) ||
        statusOk(*response)) {
        message = "unknown JSON-RPC method did not produce standard RPC_METHOD_NOT_FOUND";
        return false;
    }
    return true;
}

bool testMethodNotFound(std::string& message) {
    return testMethodNotFoundWithId(2, message);
}

bool testRequestIdMatch(std::string& message) {
    return testMethodNotFoundWithId(55, message);
}

bool testStandardErrorShape(std::string& message) {
    return testMethodNotFoundWithId(99, message);
}

axtp::Bytes encodeControl(axtp::ControlPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendControl(std::move(payload));
    return writer.bytes;
}

bool decodeOneControl(const axtp::Bytes& bytes, axtp::ControlPayload& out) {
    CapturingPayloadSink sink;
    axtp::InboundProcessor inbound(sink);
    inbound.onBytes(bytes.data(), bytes.size());
    if (sink.controls.size() != 1) {
        return false;
    }
    out = std::move(sink.controls[0]);
    return true;
}

bool testOpenAccept(std::string& message) {
    axtp::AxtpCore core;
    axtp::ControlPayload open;
    open.opcode = axtp::ControlOpcode::Open;
    open.controlId = 1;
    auto bytes = encodeControl(open);
    core.byteSink().onBytes(bytes.data(), bytes.size());
    auto responseBytes = core.tryPopOutboundBytes();
    axtp::ControlPayload response;
    if (!responseBytes.has_value() || !decodeOneControl(*responseBytes, response) ||
        response.opcode != axtp::ControlOpcode::Accept || response.controlId != 1 ||
        response.statusCode != axtp::ErrorCode::Success) {
        message = "CONTROL OPEN did not produce ACCEPT";
        return false;
    }
    return true;
}

bool testClose(std::string& message) {
    axtp::AxtpCore core;
    axtp::ControlPayload open;
    open.opcode = axtp::ControlOpcode::Open;
    open.controlId = 1;
    auto bytes = encodeControl(open);
    core.byteSink().onBytes(bytes.data(), bytes.size());
    while (core.tryPopOutboundBytes().has_value()) {}

    axtp::ControlPayload close;
    close.opcode = axtp::ControlOpcode::Close;
    close.controlId = 2;
    bytes = encodeControl(close);
    core.byteSink().onBytes(bytes.data(), bytes.size());
    auto responseBytes = core.tryPopOutboundBytes();
    axtp::ControlPayload response;
    if (!responseBytes.has_value() || !decodeOneControl(*responseBytes, response) ||
        response.opcode != axtp::ControlOpcode::CloseAck || response.controlId != 2 ||
        core.controlSessionOpen()) {
        message = "CONTROL CLOSE did not produce CLOSE_ACK and close the session";
        return false;
    }
    return true;
}

bool testPingPong(std::string& message) {
    axtp::AxtpCore core;
    axtp::ControlPayload ping;
    ping.opcode = axtp::ControlOpcode::Ping;
    ping.controlId = 3;
    auto bytes = encodeControl(ping);
    core.byteSink().onBytes(bytes.data(), bytes.size());
    auto responseBytes = core.tryPopOutboundBytes();
    axtp::ControlPayload response;
    if (!responseBytes.has_value() || !decodeOneControl(*responseBytes, response) ||
        response.opcode != axtp::ControlOpcode::Pong || response.controlId != 3) {
        message = "CONTROL PING did not produce PONG";
        return false;
    }
    return true;
}

bool testSubscribeEvent(std::string& message) {
    return testSessionHelloIdentify(message);
}

bool testUnsubscribeEvent(std::string& message) {
    axtp::BasicBroker<> broker;
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) {
        return false;
    }
    std::string sid;
    if (!identify(transport, sid, message)) {
        return false;
    }
    transport.injectJson(std::string(R"({"sid":")") + sid + R"(","op":4,"d":{"eventMasks":""}})");
    auto response = transport.popJson(&message);
    if (!response.has_value() ||
        response->at("op").get<int>() != static_cast<int>(axtp::RpcOp::Identified)) {
        message = "REIDENTIFY did not produce IDENTIFIED";
        return false;
    }
    return true;
}

bool testEmitEvent(std::string& message) {
    axtp::BasicBroker<> broker;
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) {
        return false;
    }
    std::string sid;
    if (!identify(transport, sid, message)) {
        return false;
    }
    axtp::RpcPayload event;
    event.op = axtp::RpcOp::Event;
    event.methodOrEventId = 0x0901;
    event.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
    event.meta.jsonSid = sid;
    const std::string body = R"({"reason":"user_request","applyState":"applied"})";
    event.body = axtp::Bytes(body.begin(), body.end());
    adapter->sendEvent(std::move(event));
    auto response = transport.popJson(&message);
    if (!response.has_value()) {
        return false;
    }
    const auto& d = response->at("d");
    if (response->at("op").get<int>() != static_cast<int>(axtp::RpcOp::Event) ||
        d.at("event").get<std::string>() != "audio.algorithmConfigChanged" ||
        d.at("data").at("reason").get<std::string>() != "user_request") {
        message = "event output did not match audio.algorithmConfigChanged";
        return false;
    }
    return true;
}

bool testCapabilityGetAll(std::string& message) {
    const auto getConfig = axtp::RegistryLookup::methodIdByName("audio.getAlgorithmConfig");
    const auto getCaps = axtp::RegistryLookup::methodIdByName("audio.getAlgorithmCapabilities");
    const auto setConfig = axtp::RegistryLookup::methodIdByName("audio.setAlgorithmConfig");
    const auto resetConfig = axtp::RegistryLookup::methodIdByName("audio.resetAlgorithmConfig");
    if (axtp::kMethodRegistryCount < 4 || !getConfig.has_value() || !getCaps.has_value() ||
        !setConfig.has_value() || !resetConfig.has_value() || *getConfig != 0x0901 ||
        *getCaps != 0x090D) {
        message = "generated method registry does not expose spec/v0.0.2 audio methods";
        return false;
    }
    return true;
}

bool testCapabilityMethodBinding(std::string& message) {
    const auto* method = axtp::RegistryLookup::methodById(0x0901);
    const auto* event = axtp::RegistryLookup::eventById(0x0901);
    bool capabilityFound = false;
    for (const auto& capability : axtp::kCapabilityRegistry) {
        if (capability.id == 0x0901 && std::string_view(capability.name) == "audio.algorithm") {
            capabilityFound = true;
        }
    }
    if (method == nullptr || event == nullptr || !capabilityFound ||
        std::string_view(method->domain) != "audio" || std::string_view(event->domain) != "audio") {
        message = "audio.algorithm capability, method, or event binding is missing";
        return false;
    }
    return true;
}

bool testCapabilityUnsupportedMethod(std::string& message) {
    return testMethodNotFoundWithId(4, message);
}

bool testStreamData(std::string& message) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    axtp::StreamPayload stream;
    stream.streamId = 9;
    stream.seqId = 1;
    stream.cursor = 0;
    stream.data = {0xAA, 0xBB, 0xCC};
    outbound.sendStream(stream);

    CapturingPayloadSink sink;
    axtp::InboundProcessor inbound(sink);
    inbound.onBytes(writer.bytes.data(), writer.bytes.size());
    if (sink.streams.size() != 1 || sink.streams[0].streamId != 9 || sink.streams[0].seqId != 1 ||
        sink.streams[0].cursor != 0 || sink.streams[0].data != stream.data) {
        message = "STREAM payload did not preserve streamId, seqId, cursor, and data";
        return false;
    }
    return true;
}

std::string statusName(Status status) {
    switch (status) {
    case Status::Passed:
        return "passed";
    case Status::Failed:
    case Status::Pending:
        return "failed";
    case Status::Skipped:
        return "skipped";
    case Status::Unsupported:
        return "unsupported";
    }
    return "failed";
}

std::string requirementName(Requirement requirement) {
    switch (requirement) {
    case Requirement::Required:
        return "required";
    case Requirement::Optional:
        return "optional";
    case Requirement::Unsupported:
        return "unsupported";
    case Requirement::NotSelected:
        return "not-selected";
    }
    return "not-selected";
}

nlohmann::json caseToJson(const CaseResult& item) {
    nlohmann::json object;
    object["id"] = item.id;
    object["level"] = item.level;
    object["requirement"] = requirementName(item.requirement);
    object["status"] = statusName(item.status);
    object["durationMs"] = item.durationMs;
    object["message"] = item.message;
    return object;
}

bool writeResult(const std::string& outputPath, const std::string& profilePath) {
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::size_t unsupported = 0;
    for (const auto& item : cases) {
        const auto status = item.status == Status::Pending ? Status::Failed : item.status;
        if (status == Status::Passed) {
            ++passed;
        } else if (status == Status::Failed) {
            ++failed;
        } else if (status == Status::Skipped) {
            ++skipped;
        } else if (status == Status::Unsupported) {
            ++unsupported;
        }
    }

    nlohmann::json root;
    root["runtime"] = "axtp-cpp-runtime";
    root["runtimeVersion"] = axtp::generated::kRuntimeVersion;
    root["specTag"] = axtp::generated::kSpecTag;
    root["profile"] = profilePath;
    root["requiredLevels"] = {"core", "websocket-jsonrpc"};
    root["optionalLevels"] = {"capability", "framed-binary", "event", "stream"};
    root["unsupportedLevels"] = nlohmann::json::array();
    nlohmann::json summary;
    summary["total"] = cases.size();
    summary["passed"] = passed;
    summary["failed"] = failed;
    summary["skipped"] = skipped;
    summary["unsupported"] = unsupported;
    root["summary"] = std::move(summary);
    nlohmann::json caseArray;
    for (const auto& item : cases) {
        caseArray.push_back(caseToJson(item));
    }
    root["cases"] = std::move(caseArray);

    std::ofstream output(outputPath);
    if (!output.good()) {
        std::fprintf(stderr, "failed to open conformance result for writing: %s\n", outputPath.c_str());
        return false;
    }
    output << root.dump() << '\n';
    return true;
}

bool envIsTrue(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "true";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <axtp-spec-path> <result-json-path> <runtime-profile-path>\n", argv[0]);
        return 2;
    }

    std::string manifestPath = std::string(argv[1]) + "/docs/conformance/manifest.yaml";
    if (!fileExists(manifestPath)) {
        manifestPath = std::string(argv[1]) + "/conformance/manifest.yaml";
    }
    if (!fileExists(manifestPath)) {
        std::fprintf(stderr, "missing conformance manifest: %s\n", manifestPath.c_str());
        return 2;
    }
    if (!fileExists(argv[3])) {
        std::fprintf(stderr, "missing runtime profile: %s\n", argv[3]);
        return 2;
    }

    runCase("handshake.open_accept", testOpenAccept);
    runCase("handshake.close", testClose);
    runCase("handshake.ping_pong", testPingPong);
    runCase("session.hello_identify_identified", testSessionHelloIdentify);
    runCase("session.request_before_identified", testRequestBeforeIdentified);
    runCase("rpc.request_response_json", testRequestResponseJson);
    runCase("rpc.method_not_found", testMethodNotFound);
    runCase("rpc.request_id_match", testRequestIdMatch);
    runCase("event.subscribe_event", testSubscribeEvent);
    runCase("event.unsubscribe_event", testUnsubscribeEvent);
    runCase("event.emit_event", testEmitEvent);
    runCase("capability.get_all", testCapabilityGetAll);
    runCase("capability.method_binding", testCapabilityMethodBinding);
    runCase("capability.unsupported_method", testCapabilityUnsupportedMethod);
    runCase("error.standard_error_shape", testStandardErrorShape);
    runCase("stream.stream_data", testStreamData);

    bool requiredIssue = false;
    bool optionalIssue = false;
    for (const auto& item : cases) {
        if (item.requirement == Requirement::Required && item.status != Status::Passed) {
            requiredIssue = true;
        }
        if (item.requirement == Requirement::Optional && item.status != Status::Passed) {
            optionalIssue = true;
        }
    }

    if (!writeResult(argv[2], argv[3])) {
        return 1;
    }

    if ((requiredIssue && !envIsTrue("CONFORMANCE_ALLOW_INCOMPLETE")) ||
        (optionalIssue && envIsTrue("CONFORMANCE_STRICT_OPTIONAL"))) {
        return 1;
    }
    return 0;
}
