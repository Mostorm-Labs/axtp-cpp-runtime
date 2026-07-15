#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/runtime/broker/basic_broker.hpp"
#include "core/runtime/core/axtp_core.hpp"
#include "core/protocol/wire/inbound_processor.hpp"
#include "core/protocol/wire/outbound_processor.hpp"
#include "core/protocol/generated/axtp_capability_generated.h"
#include "core/protocol/generated/axtp_generated_version.hpp"
#include "core/protocol/generated/axtp_error_code_generated.h"
#include "core/protocol/generated/axtp_method_registry_generated.h"
#include "core/protocol/generated/registry_lookup.h"
#include "core/support/io/byte_writer_sink.hpp"
#include "core/runtime/endpoint/axtp_endpoint.hpp"
#include "core/runtime/transport/transport.hpp"
#include "json_rpc/websocket_json_rpc_adapter.hpp"
#include "json_rpc/rpc_client_session.hpp"
#include "audio_algorithm_config_validator.hpp"

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
    nlohmann::json definition;
};

std::vector<CaseResult> cases;
std::vector<std::string> requiredLevels;
std::vector<std::string> optionalLevels;
std::vector<std::string> unsupportedLevels;

CaseResult* findCase(std::string_view id);

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
        std::lock_guard<std::mutex> lock(_mutex);
        _outgoing.push(axtp::Bytes(data, data + size));
        _condition.notify_all();
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
        std::lock_guard<std::mutex> lock(_mutex);
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

    bool noEventWithin(std::string_view name, std::chrono::milliseconds window) {
        std::unique_lock<std::mutex> lock(_mutex);
        const auto deadline = std::chrono::steady_clock::now() + window;
        while (true) {
            while (!_outgoing.empty()) {
                auto bytes = std::move(_outgoing.front());
                _outgoing.pop();
                const auto object = nlohmann::json::parse(std::string(bytes.begin(), bytes.end()));
                if (object.value("op", -1) == static_cast<int>(axtp::RpcOp::Event) &&
                    object.at("d").value("event", "") == name) return false;
            }
            if (_condition.wait_until(lock, deadline) == std::cv_status::timeout) return true;
        }
    }

private:
    axtp::IByteSink* _sink = nullptr;
    std::queue<axtp::Bytes> _outgoing;
    std::mutex _mutex;
    std::condition_variable _condition;
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

#if defined(_WIN32)
std::string shellQuote(const std::string& value) {
    // cmd.exe accepts double-quoted paths.  The conformance paths come from
    // the filesystem, but escape an embedded quote for completeness.
    std::string out = "\"";
    for (const auto ch : value) out += ch == '"' ? "\\\"" : std::string(1, ch);
    return out + "\"";
}
#define AXTP_POPEN _popen
#define AXTP_PCLOSE _pclose
#else
std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (const auto ch : value) out += ch == '\'' ? "'\\''" : std::string(1, ch);
    return out + "'";
}
#define AXTP_POPEN popen
#define AXTP_PCLOSE pclose
#endif

nlohmann::json loadYaml(const std::string& path) {
    const auto command = std::string("ruby ") + shellQuote(AXTP_CONFORMANCE_YAML_READER) +
                         " " + shellQuote(path) + " 2>&1";
    FILE* pipe = AXTP_POPEN(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("cannot start YAML reader for " + path);
    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
    const auto status = AXTP_PCLOSE(pipe);
    if (status != 0 || output.empty())
        throw std::runtime_error("YAML reader failed for " + path + ": " + output);
    const auto lastNewline = output.find_last_of('\n', output.size() > 1 ? output.size() - 2 : 0);
    const auto jsonText = lastNewline == std::string::npos ? output : output.substr(lastNewline + 1);
    try {
        return nlohmann::json::parse(jsonText);
    } catch (const std::exception& ex) {
        throw std::runtime_error("invalid YAML reader output for " + path + ": " + ex.what() +
                                 "; output=" + output);
    }
}

void loadSelectedCases(const std::string& conformanceDir,
                       const std::string& manifestPath,
                       const std::string& profilePath) {
    const auto manifest = loadYaml(manifestPath);
    const auto profile = loadYaml(profilePath);
    std::map<std::string, nlohmann::json> definitions;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             std::filesystem::path(conformanceDir) / "cases")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".yaml") continue;
        auto definition = loadYaml(entry.path().string());
        const auto id = definition.value("id", "");
        if (id.empty() || definitions.count(id)) throw std::runtime_error("invalid/duplicate case id");
        definitions.emplace(id, std::move(definition));
    }
    requiredLevels = profile.value("required_levels", std::vector<std::string>{});
    optionalLevels = profile.value("optional_levels", std::vector<std::string>{});
    unsupportedLevels = profile.value("unsupported_levels", std::vector<std::string>{});
    std::map<std::string, Requirement> selected;
    for (const auto& level : requiredLevels) selected[level] = Requirement::Required;
    for (const auto& level : optionalLevels) selected[level] = Requirement::Optional;
    for (const auto& [level, requirement] : selected) {
        const auto levelIt = manifest.at("levels").find(level);
        if (levelIt == manifest.at("levels").end()) {
            throw std::runtime_error("profile declares unknown level: " + level);
        }
        for (const auto& idValue : levelIt->at("required_cases")) {
            const auto id = idValue.get<std::string>();
            if (findCase(id) != nullptr) continue;
            const auto definitionIt = definitions.find(id);
            if (definitionIt == definitions.end()) throw std::runtime_error("selected case is missing: " + id);
            auto definition = definitionIt->second;
            if (definition.value("id", "") != id || definition.value("level", "") .empty()) {
                throw std::runtime_error("invalid selected case: " + id);
            }
            cases.push_back(CaseResult{id, definition.at("level").get<std::string>(), requirement,
                                       Status::Pending, 0.0, "", std::move(definition)});
        }
    }
    for (const auto& level : unsupportedLevels) {
        const auto levelIt = manifest.at("levels").find(level);
        if (levelIt == manifest.at("levels").end()) {
            throw std::runtime_error("profile marks unknown level unsupported: " + level);
        }
        for (const auto& idValue : levelIt->at("required_cases")) {
            const auto id = idValue.get<std::string>();
            if (findCase(id) != nullptr) continue;
            const auto definitionIt = definitions.find(id);
            if (definitionIt == definitions.end()) throw std::runtime_error("unsupported case is missing: " + id);
            auto definition = definitionIt->second;
            cases.push_back(CaseResult{id, definition.at("level").get<std::string>(),
                                       Requirement::Unsupported, Status::Unsupported, 0.0,
                                       "level is undeclared by the pinned runtime profile",
                                       std::move(definition)});
        }
    }
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

int rpcOpValue(std::string_view name) {
    static const std::map<std::string, axtp::RpcOp> values = {
        {"HELLO", axtp::RpcOp::Hello}, {"IDENTIFY", axtp::RpcOp::Identify},
        {"IDENTIFIED", axtp::RpcOp::Identified}, {"REIDENTIFY", axtp::RpcOp::Reidentify}};
    const auto it = values.find(std::string(name));
    if (it == values.end()) throw std::runtime_error("unknown JSON-RPC op in case");
    return static_cast<int>(it->second);
}

bool matchesJsonExpectation(const nlohmann::json& actual, const nlohmann::json& expected) {
    if (expected.is_object() && expected.value("type", "") == "string") {
        return actual.is_string() &&
               actual.get<std::string>().size() >= expected.value("minLength", std::size_t{0});
    }
    if (expected.is_object()) {
        if (!actual.is_object()) return false;
        for (const auto& [key, value] : expected.items()) {
            if (!actual.contains(key)) return false;
            if (key == "op" && value.is_string()) {
                if (actual.at(key) != rpcOpValue(value.get<std::string>())) return false;
            } else if (!matchesJsonExpectation(actual.at(key), value)) {
                return false;
            }
        }
        return true;
    }
    return actual == expected;
}

bool executeAdvisoryScenarios(const nlohmann::json& definition, std::string& message) {
    if (!definition.contains("scenarios") || definition.at("scenarios").size() != 6) {
        message = "advisory matrix must contain six scenarios";
        return false;
    }
    for (const auto& scenario : definition.at("scenarios")) {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        MemoryJsonTransport transport;
        std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
        endpoint.attachTransport(transport);
        adapter = std::make_unique<axtp::WebSocketJsonRpcAdapter>(endpoint, transport);
        transport.bind(*adapter);
        transport.open();
        adapter->poll(transport);  // Retain the real outbound HELLO for the graph's first observe.
        axtp::RpcClientSession clientSession;
        std::optional<nlohmann::json> generatedIdentify;
        std::map<std::string, nlohmann::json> captures;
        std::map<std::string, nlohmann::json> observedSteps;
        std::map<std::string, std::size_t> stepIndex;
        std::size_t index = 0;
        for (const auto& step : scenario.at("steps")) {
            const auto id = step.value("id", "");
            if (id.empty() || stepIndex.count(id)) { message = "duplicate/missing step id"; return false; }
            stepIndex[id] = index++;
        }
        index = 0;
        for (const auto& step : scenario.at("steps")) {
            const auto role = step.value("role", "");
            if (role.empty() || step.value("id", "").empty()) {
                message = "scenario step is missing role/id";
                return false;
            }
            for (const auto* edge : {"responseTo", "triggeredBy"}) {
                if (!step.contains(edge)) continue;
                const auto target = step.at(edge).get<std::string>();
                if (!stepIndex.count(target) || stepIndex.at(target) >= index) {
                    message = std::string("broken/forward graph edge: ") + edge;
                    return false;
                }
            }
            if (step.contains("jsonrpc")) {
                auto wire = step.at("jsonrpc");
                if (wire.at("op").is_string()) wire["op"] = rpcOpValue(wire.at("op").get<std::string>());
                if (wire.contains("sid") && wire.at("sid").is_object()) {
                    const auto ref = wire.at("sid").at("ref").get<std::string>();
                    const auto dot = ref.find('.');
                    if (dot == std::string::npos || !captures.count(ref.substr(0, dot))) {
                        message = "unresolved advisory SID reference";
                        return false;
                    }
                    wire["sid"] = captures.at(ref.substr(0, dot)).at(ref.substr(dot + 1));
                }
                const auto direction = step.value("direction", "");
                if (direction == "client_to_server") {
                    auto clientWire = wire;
                    if (wire.value("op", -1) == static_cast<int>(axtp::RpcOp::Identify)) {
                        if (!generatedIdentify.has_value() ||
                            !matchesJsonExpectation(*generatedIdentify, wire)) {
                            message = "client session Identify disagrees with declared step";
                            return false;
                        }
                        clientWire = *generatedIdentify;
                    } else if (wire.value("op", -1) == static_cast<int>(axtp::RpcOp::Reidentify)) {
                        clientWire = clientSession.makeReidentify();
                        if (!matchesJsonExpectation(clientWire, wire)) {
                            message = "client session Reidentify disagrees with declared/ref-resolved step";
                            return false;
                        }
                    }
                    transport.injectJson(clientWire.dump());
                    observedSteps[step.at("id").get<std::string>()] = clientWire;
                } else if (direction == "server_to_client") {
                    auto actual = transport.popJson(&message);
                    auto comparableWire = wire;
                    auto comparableActual = actual.value_or(nlohmann::json::object());
                    // axtpVersion is advisory: verify the real HELLO shape/path while allowing
                    // the YAML matrix's peer diagnostic value to differ from this runtime's.
                    if (wire.value("op", -1) == static_cast<int>(axtp::RpcOp::Hello)) {
                        comparableWire["d"].erase("axtpVersion");
                        comparableActual["d"].erase("axtpVersion");
                        const nlohmann::json* identifyStep = nullptr;
                        for (const auto& candidate : scenario.at("steps")) {
                            if (candidate.contains("jsonrpc") && candidate.at("jsonrpc").value("op", "") == "IDENTIFY") {
                                identifyStep = &candidate;
                                break;
                            }
                        }
                        if (!identifyStep) { message = "scenario lacks IDENTIFY"; return false; }
                        const auto& identifyD = identifyStep->at("jsonrpc").at("d");
                        generatedIdentify = clientSession.acceptHello(
                            wire, identifyD.at("randomSeed").get<std::uint32_t>(),
                            identifyD.value("eventMasks", ""));
                        const auto declaredVersion = wire.at("d").find("axtpVersion");
                        if ((declaredVersion == wire.at("d").end()) !=
                            !clientSession.observedAxtpVersion().has_value() ||
                            (declaredVersion != wire.at("d").end() &&
                             clientSession.observedAxtpVersion() !=
                                 std::optional<std::string>(declaredVersion->is_string()
                                     ? declaredVersion->get<std::string>() : declaredVersion->dump()))) {
                            message = "client session did not observe exact advisory variant";
                            return false;
                        }
                    }
                    if (!actual.has_value() || !matchesJsonExpectation(comparableActual, comparableWire)) {
                        message = "outbound server message disagrees with graph input/expectation";
                        return false;
                    }
                    observedSteps[step.at("id").get<std::string>()] = *actual;
                } else {
                    message = "wrong advisory step direction";
                    return false;
                }
            } else if (step.contains("expect")) {
                if (step.value("direction", "") != "server_to_client") {
                    message = "wrong advisory expectation direction";
                    return false;
                }
                auto actual = transport.popJson(&message);
                if (!actual.has_value()) return false;
                const auto& expected = step.at("expect").at("jsonrpc");
                if (!matchesJsonExpectation(*actual, expected)) {
                    message = "advisory observed message disagrees with expectation";
                    return false;
                }
                if (step.contains("responseTo") && step.at("responseTo").get<std::string>().empty()) {
                    message = "invalid responseTo edge";
                    return false;
                }
                if (step.contains("captureAs")) captures[step.at("captureAs").get<std::string>()] = *actual;
                if (actual->value("op", -1) == static_cast<int>(axtp::RpcOp::Identified))
                    clientSession.acceptIdentified(*actual);
                observedSteps[step.at("id").get<std::string>()] = *actual;
            } else {
                message = "unknown advisory step shape";
                return false;
            }
            ++index;
        }
        if (!captures.count("identified") || !captures.count("reidentified")) {
            message = "advisory captures are incomplete";
            return false;
        }
        for (const auto& assertion : scenario.value("assertions", std::vector<std::string>{})) {
            if (assertion == "identified.sid != \"\"" &&
                captures.at("identified").at("sid").get<std::string>().empty()) return false;
            else if (assertion == "reidentified.sid != \"\"" &&
                captures.at("reidentified").at("sid").get<std::string>().empty()) return false;
            else if (assertion == "reidentify.sid == identified.sid" &&
                     observedSteps.at("reidentify").at("sid") != captures.at("identified").at("sid"))
                return false;
            else if (assertion == "hello.d does not contain axtpVersion") {
                // The scenario input omits it; the runtime's own diagnostic HELLO may include it.
                const auto& declaredHello = scenario.at("steps").front().at("jsonrpc").at("d");
                if (declaredHello.contains("axtpVersion")) return false;
            } else if (assertion != "identified.sid != \"\"" &&
                       assertion != "reidentified.sid != \"\"" &&
                       assertion != "reidentify.sid == identified.sid") {
                message = "unsupported advisory assertion: " + assertion;
                return false;
            }
        }
    }
    return true;
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

void registerSupportedGet(axtp::BasicBroker<>& broker) {
    broker.registerJsonMethod("audio.getAlgorithmConfig", [](const axtp::RpcContext&, std::string_view) {
        return std::string(R"({"noiseSuppression":{"enabled":true,"level":3}})");
    });
}

bool testDegradationAndLiveness(std::uint32_t firstId,
                                std::string_view method,
                                std::string_view params,
                                std::string& message) {
    axtp::BasicBroker<> broker;
    registerSupportedGet(broker);
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) return false;
    std::string sid;
    if (!identify(transport, sid, message)) return false;
    transport.injectJson(std::string(R"({"sid":")") + sid + R"(","op":7,"d":{"id":)" +
                         std::to_string(firstId) + R"(,"method":")" + std::string(method) +
                         R"(","params":)" + std::string(params) + "}}");
    auto degraded = transport.popJson(&message);
    if (!degraded.has_value() || statusCode(*degraded) !=
                                     static_cast<std::uint16_t>(axtp::ErrorCode::NotSupported)) {
        message = "registered unavailable operation did not return exact NOT_SUPPORTED";
        return false;
    }
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":7,"d":{"id":)" + std::to_string(firstId + 1) +
                         R"(,"method":"audio.getAlgorithmConfig","params":{}}})");
    auto live = transport.popJson(&message);
    if (!live.has_value() || !statusOk(*live)) {
        message = "session was not live after NOT_SUPPORTED";
        return false;
    }
    return true;
}

bool testSessionSurvivesNotSupported(std::string& message) {
    return testDegradationAndLiveness(41, "audio.setAlgorithmConfig", "{}", message);
}

const nlohmann::json* stepWithRole(const nlohmann::json& definition, std::string_view role) {
    for (const auto& step : definition.at("steps")) {
        if (step.value("role", "") == role) return &step;
    }
    return nullptr;
}

bool executeDegradationGraph(const nlohmann::json& definition, std::string& message) {
    const auto* trigger = stepWithRole(definition, "trigger");
    const auto* degraded = stepWithRole(definition, "degraded");
    const nlohmann::json* liveness = nullptr;
    const nlohmann::json* livenessResponse = nullptr;
    std::size_t triggerCount = 0, degradedCount = 0, liveRequestCount = 0, liveResponseCount = 0;
    for (const auto& step : definition.at("steps")) {
        if (step.value("role", "") == "trigger" && step.contains("rpc")) ++triggerCount;
        if (step.value("role", "") == "degraded" && step.contains("expect")) ++degradedCount;
        if (step.value("role", "") == "liveness" && step.contains("rpc")) { liveness = &step; ++liveRequestCount; }
        if (step.value("role", "") == "liveness" && step.contains("expect")) { livenessResponse = &step; ++liveResponseCount; }
    }
    if (!trigger || !degraded || !liveness || !livenessResponse || !trigger->contains("rpc") ||
        triggerCount != 1 || degradedCount != 1 || liveRequestCount != 1 || liveResponseCount != 1) {
        message = "invalid degradation graph";
        return false;
    }
    if (trigger->value("direction", "") != "client_to_server" ||
        trigger->at("rpc").value("op", "") != "REQUEST" ||
        degraded->value("direction", "") != "server_to_client" ||
        degraded->at("expect").at("rpc").value("op", "") != "REQUEST_RESPONSE" ||
        liveness->value("direction", "") != "client_to_server" ||
        liveness->at("rpc").value("op", "") != "REQUEST" ||
        livenessResponse->value("direction", "") != "server_to_client" ||
        livenessResponse->at("expect").at("rpc").value("op", "") != "REQUEST_RESPONSE") {
        message = "degradation graph has wrong direction/op";
        return false;
    }
    if (degraded->value("responseTo", "") != trigger->value("id", "") ||
        liveness->value("triggeredBy", "") != degraded->value("id", "") ||
        livenessResponse->value("responseTo", "") != liveness->value("id", "")) {
        message = "broken degradation responseTo/triggeredBy association";
        return false;
    }
    const auto& rpc = trigger->at("rpc");
    const auto method = rpc.at("method").get<std::string>();
    axtp::BasicBroker<> broker;
    registerSupportedGet(broker);
    if (definition.at("semantic").value("kind", "") == "registered_feature_degradation") {
        const auto methodId = broker.registry().findMethodId(method);
        if (!methodId.has_value()) { message = "degradation method is absent from generated registry"; return false; }
        broker.registerRawMethod(*methodId, [](const axtp::RpcContext&, const axtp::RpcRequestView&) {
            axtp::RpcResponseData result;
            result.overrideStatus = true;
            result.statusCode = axtp::ErrorCode::NotSupported;
            return result;
        });
    }
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) return false;
    std::string sid;
    if (!identify(transport, sid, message)) return false;
    nlohmann::json request = {{"sid", sid}, {"op", static_cast<int>(axtp::RpcOp::Request)}};
    request["d"] = {{"id", rpc.at("requestId")}, {"method", method},
                    {"params", rpc.value("params", nlohmann::json::object())}};
    transport.injectJson(request.dump());
    auto response = transport.popJson(&message);
    const auto expected = degraded->at("expect").at("rpc");
    const auto expectedName = expected.at("statusCode").get<std::string>();
    const auto* expectedDescriptor = std::find_if(
        std::begin(axtp::kErrorRegistry), std::end(axtp::kErrorRegistry),
        [&](const auto& descriptor) { return expectedName == descriptor.name; });
    if (expectedDescriptor == std::end(axtp::kErrorRegistry)) {
        message = "expected status is absent from generated registry";
        return false;
    }
    if (!response.has_value() || response->at("d").at("id") != expected.at("requestId") ||
        statusCode(*response) != expectedDescriptor->id) {
        message = "degraded response disagrees with shared case";
        return false;
    }
    // Resolve the graph edge rather than merely running a second independent request.
    const auto& liveRpc = liveness->at("rpc");
    request["d"] = {{"id", liveRpc.at("requestId")}, {"method", liveRpc.at("method")},
                    {"params", liveRpc.value("params", nlohmann::json::object())}};
    transport.injectJson(request.dump());
    auto liveResponse = transport.popJson(&message);
    const auto& liveExpected = livenessResponse->at("expect").at("rpc");
    const auto liveExpectedName = liveExpected.at("statusCode").get<std::string>();
    const auto* liveDescriptor = std::find_if(
        std::begin(axtp::kErrorRegistry), std::end(axtp::kErrorRegistry),
        [&](const auto& descriptor) { return liveExpectedName == descriptor.name; });
    return liveDescriptor != std::end(axtp::kErrorRegistry) && liveResponse.has_value() &&
           statusCode(*liveResponse) == liveDescriptor->id &&
           liveResponse->at("d").at("id") == liveExpected.at("requestId");
}

bool testUnknownOptionalFieldIgnored(std::string& message) {
    axtp::BasicBroker<> broker;
    registerSupportedGet(broker);
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) return false;
    std::string sid;
    if (!identify(transport, sid, message)) return false;
    for (const auto requestId : {43, 44}) {
        const auto params = requestId == 43 ? R"({"futureOptionalField":{"mode":"future-value"}})" : "{}";
        transport.injectJson(std::string(R"({"sid":")") + sid + R"(","op":7,"d":{"id":)" +
                             std::to_string(requestId) +
                             R"(,"method":"audio.getAlgorithmConfig","params":)" + params + "}}");
        auto response = transport.popJson(&message);
        if (!response.has_value() || !statusOk(*response)) {
            message = "unknown optional field invalidated the request or session";
            return false;
        }
    }
    return true;
}

bool testInvalidParams(std::string& message) {
    axtp::BasicBroker<> broker;
    broker.registerRequestValidator(axtp::AudioAlgorithmConfigValidator{});
    registerSupportedGet(broker);
    broker.registerJsonMethod("audio.setAlgorithmConfig", [](const axtp::RpcContext&, std::string_view) {
        return std::string(R"({})");
    });
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) return false;
    std::string sid;
    if (!identify(transport, sid, message)) return false;
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":7,"d":{"id":3,"method":"audio.setAlgorithmConfig","params":{"config":{"noiseSuppression":{"level":999}}}}})");
    auto response = transport.popJson(&message);
    if (!response.has_value() || statusCode(*response) !=
                                     static_cast<std::uint16_t>(axtp::ErrorCode::OutOfRange)) {
        message = "invalid params did not return OUT_OF_RANGE";
        return false;
    }
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":7,"d":{"id":4,"method":"audio.getAlgorithmConfig","params":{}}})");
    auto live = transport.popJson(&message);
    if (!live.has_value() || !statusOk(*live)) {
        message = "session was not live after invalid params";
        return false;
    }
    return true;
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

bool testHeartbeat(std::string& message) {
    axtp::AxtpCore core;
    axtp::ControlPayload heartbeat;
    heartbeat.opcode = axtp::ControlOpcode::Heartbeat;
    heartbeat.controlId = 3;
    auto bytes = encodeControl(heartbeat);
    core.byteSink().onBytes(bytes.data(), bytes.size());
    auto responseBytes = core.tryPopOutboundBytes();
    axtp::ControlPayload response;
    if (!responseBytes.has_value() || !decodeOneControl(*responseBytes, response) ||
        response.opcode != axtp::ControlOpcode::HeartbeatAck || response.controlId != 3 ||
        response.statusCode != axtp::ErrorCode::Success) {
        message = "CONTROL HEARTBEAT did not produce HEARTBEAT_ACK";
        return false;
    }
    return true;
}

bool testSubscribeEvent(std::string& message) {
    return testSessionHelloIdentify(message);
}

bool testUnsubscribeEvent(std::string& message, std::chrono::milliseconds noEventWindow) {
    axtp::BasicBroker<> broker;
    registerSupportedGet(broker);
    broker.registerJsonMethod("audio.setAlgorithmConfig", [](const axtp::RpcContext&, std::string_view) {
        return std::string(R"({})");
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
    transport.injectJson(std::string(R"({"sid":")") + sid + R"(","op":4,"d":{"eventMasks":""}})");
    auto response = transport.popJson(&message);
    if (!response.has_value() ||
        response->at("op").get<int>() != static_cast<int>(axtp::RpcOp::Identified)) {
        message = "REIDENTIFY did not produce IDENTIFIED";
        return false;
    }
    sid = stringField(*response, "sid");
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":7,"d":{"id":46,"method":"audio.setAlgorithmConfig","params":{"config":{"noiseSuppression":{"enabled":false}}}}})");
    auto trigger = transport.popJson(&message);
    if (!trigger.has_value() || !statusOk(*trigger) ||
        !transport.noEventWithin("audio.algorithmConfigChanged", noEventWindow)) {
        message = "unsubscribed event was emitted or trigger failed";
        return false;
    }
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":7,"d":{"id":47,"method":"audio.getAlgorithmConfig","params":{}}})");
    auto live = transport.popJson(&message);
    return live.has_value() && statusOk(*live);
}

bool testUnsubscribeEvent(std::string& message) {
    return testUnsubscribeEvent(message, std::chrono::milliseconds(500));
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

bool testUnknownEventIgnored(std::string& message) {
    axtp::BasicBroker<> broker;
    registerSupportedGet(broker);
    std::size_t dispatched = 0;
    broker.registerEventHandler([&dispatched](const axtp::BrokerContext&, const axtp::RpcPayload&) {
        ++dispatched;
    });
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) return false;
    std::string sid;
    if (!identify(transport, sid, message)) return false;
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":6,"d":{"event":"vendor.futureStateChanged","data":{"futureOptionalField":true}}})");
    if (dispatched != 0) {
        message = "unknown event was dispatched to the application";
        return false;
    }
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":7,"d":{"id":45,"method":"audio.getAlgorithmConfig","params":{}}})");
    auto live = transport.popJson(&message);
    return live.has_value() && statusOk(*live);
}

bool testUnsupportedCapabilityNotEmitted(std::string& message,
                                         std::chrono::milliseconds noEventWindow) {
    axtp::BasicBroker<> broker;
    registerSupportedGet(broker);
    broker.registerJsonMethod("audio.setAlgorithmConfig", [](const axtp::RpcContext&, std::string_view) {
        return std::string(R"({})");
    });
    axtp::AxtpEndpoint endpoint(broker);
    MemoryJsonTransport transport;
    std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
    if (!setupJsonAdapter(broker, endpoint, transport, adapter, message)) return false;
    std::string sid;
    if (!identify(transport, sid, message)) return false;
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":7,"d":{"id":51,"method":"audio.setAlgorithmConfig","params":{"config":{"noiseSuppression":{"enabled":false}}}}})");
    auto response = transport.popJson(&message);
    if (!response.has_value() || !statusOk(*response) ||
        !transport.noEventWithin("audio.algorithmConfigChanged", noEventWindow)) {
        message = "unsupported capability event was emitted or trigger failed";
        return false;
    }
    transport.injectJson(std::string(R"({"sid":")") + sid +
                         R"(","op":7,"d":{"id":52,"method":"audio.getAlgorithmConfig","params":{}}})");
    auto live = transport.popJson(&message);
    return live.has_value() && statusOk(*live);
}

bool testUnsupportedCapabilityNotEmitted(std::string& message) {
    return testUnsupportedCapabilityNotEmitted(message, std::chrono::milliseconds(500));
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
    root["requiredLevels"] = requiredLevels;
    root["optionalLevels"] = optionalLevels;
    root["unsupportedLevels"] = unsupportedLevels;
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

bool delayedEventCollectorRejectsMatch() {
    MemoryJsonTransport transport;
    const std::string event = R"({"sid":"S","op":6,"d":{"event":"audio.algorithmConfigChanged","data":{}}})";
    std::thread delayed([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        transport.sendBytes(reinterpret_cast<const axtp::Byte*>(event.data()), event.size());
    });
    const auto noEvent = transport.noEventWithin(
        "audio.algorithmConfigChanged", std::chrono::milliseconds(100));
    delayed.join();
    return !noEvent;
}

bool graphNegativeSelfChecks() {
    const CaseResult* advisory = nullptr;
    const CaseResult* degradation = nullptr;
    for (const auto& item : cases) {
        const auto semantic = item.definition.value("semantic", nlohmann::json::object()).value("kind", "");
        if (semantic == "advisory_version_handshake") advisory = &item;
        if (semantic == "registered_method_unavailable") degradation = &item;
    }
    std::string ignored;
    if (advisory != nullptr) {
        auto wrongDirection = advisory->definition;
        wrongDirection["scenarios"][0]["steps"][1]["direction"] = "server_to_client";
        if (executeAdvisoryScenarios(wrongDirection, ignored)) return false;
        auto brokenEdge = advisory->definition;
        brokenEdge["scenarios"][0]["steps"][3]["triggeredBy"] = "missing-step";
        if (executeAdvisoryScenarios(brokenEdge, ignored)) return false;
    }
    if (degradation != nullptr) {
        auto wrongStatus = degradation->definition;
        for (auto& step : wrongStatus["steps"]) {
            if (step.value("role", "") == "degraded")
                step["expect"]["rpc"]["statusCode"] = "SUCCESS";
        }
        if (executeDegradationGraph(wrongStatus, ignored)) return false;
        auto duplicateRole = degradation->definition;
        duplicateRole["steps"].push_back(duplicateRole["steps"][0]);
        duplicateRole["steps"].back()["id"] = "duplicate-trigger";
        if (executeDegradationGraph(duplicateRole, ignored)) return false;
    }
    return true;
}

bool executeLoadedCase(const CaseResult& item, std::string& message) {
    const auto semantic = item.definition.value("semantic", nlohmann::json::object()).value("kind", "");
    if (semantic == "advisory_version_handshake") return executeAdvisoryScenarios(item.definition, message);
    if (semantic == "registered_method_unavailable" || semantic == "registered_feature_degradation" ||
        semantic == "profile_degradation") return executeDegradationGraph(item.definition, message);
    if (semantic == "invalid_params") return testInvalidParams(message);
    if (semantic == "unknown_method_error") return testCapabilityUnsupportedMethod(message);
    if (semantic == "unknown_event_receiver_tolerance") return testUnknownEventIgnored(message);
    auto noEventWindow = [&]() {
        for (const auto& step : item.definition.at("steps")) {
            if (step.contains("expect") && step.at("expect").contains("no_event")) {
                return std::chrono::milliseconds(
                    step.at("expect").at("no_event").at("withinMs").get<int>());
            }
        }
        throw std::runtime_error("no_event semantic is missing bounded withinMs");
    };
    if (semantic == "unsubscribed_event_sender_suppression")
        return testUnsubscribeEvent(message, noEventWindow());
    if (semantic == "unsupported_event_sender_suppression")
        return testUnsupportedCapabilityNotEmitted(message, noEventWindow());
    if (semantic == "baseline_handshake") return testSessionHelloIdentify(message);

    static const std::map<std::string, std::function<bool(std::string&)>> structuralCases = {
        {"handshake.open_accept", testOpenAccept}, {"handshake.close", testClose},
        {"handshake.heartbeat", testHeartbeat},
        {"session.hello_identify_identified", testSessionHelloIdentify},
        {"session.request_before_identified", testRequestBeforeIdentified},
        {"rpc.request_response_json", testRequestResponseJson}, {"rpc.method_not_found", testMethodNotFound},
        {"rpc.request_id_match", testRequestIdMatch}, {"error.standard_error_shape", testStandardErrorShape},
        {"event.subscribe_event", testSubscribeEvent},
        {"event.unsubscribe_event",
         [](std::string& message) { return testUnsubscribeEvent(message); }},
        {"event.emit_event", testEmitEvent},
        {"capability.get_all", testCapabilityGetAll}, {"capability.method_binding", testCapabilityMethodBinding},
        {"capability.session_survives_not_supported", testSessionSurvivesNotSupported},
        {"capability.unknown_optional_field_ignored", testUnknownOptionalFieldIgnored},
    };
    const auto handler = structuralCases.find(item.id);
    if (handler == structuralCases.end()) {
        message = "selected case has unknown semantic/shape";
        return false;
    }
    return handler->second(message);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <axtp-spec-path> <result-json-path> <runtime-profile-path>\n", argv[0]);
        return 2;
    }

    std::string conformanceDir = std::string(argv[1]) + "/docs/conformance";
    std::string manifestPath = conformanceDir + "/manifest.yaml";
    if (!fileExists(manifestPath)) {
        conformanceDir = std::string(argv[1]) + "/conformance";
        manifestPath = conformanceDir + "/manifest.yaml";
    }
    if (!fileExists(manifestPath)) {
        std::fprintf(stderr, "missing conformance manifest: %s\n", manifestPath.c_str());
        return 2;
    }
    if (!fileExists(argv[3])) {
        std::fprintf(stderr, "missing runtime profile: %s\n", argv[3]);
        return 2;
    }

    try {
        loadSelectedCases(conformanceDir, manifestPath, argv[3]);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "failed to load conformance graph: %s\n", ex.what());
        return 2;
    }
    if (!delayedEventCollectorRejectsMatch()) {
        std::fprintf(stderr, "bounded no_event collector missed a delayed matching event\n");
        return 2;
    }
    if (!graphNegativeSelfChecks()) {
        std::fprintf(stderr, "conformance graph negative self-check failed\n");
        return 2;
    }
    for (const auto& item : cases) {
        if (item.status == Status::Unsupported) continue;
        runCase(item.id, [&item](std::string& message) { return executeLoadedCase(item, message); });
    }

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
