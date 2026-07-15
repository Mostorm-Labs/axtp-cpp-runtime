#include <cassert>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "core/protocol/wire/websocket_json_rpc/inbound/json_rpc_decoder.hpp"
#include "core/runtime/broker/basic_broker.hpp"
#include "core/runtime/endpoint/axtp_endpoint.hpp"
#include "core/runtime/testing/mock_transport.hpp"
#include "json_rpc/websocket_json_rpc_adapter.hpp"

namespace {

struct CapturingPayloadSink final : axtp::IPayloadSink {
  std::vector<axtp::RpcPayload> rpcs;

  void onControl(axtp::ControlPayload) override {}

  void onRpc(axtp::RpcPayload payload) override {
    rpcs.push_back(std::move(payload));
  }

  void onStream(axtp::StreamPayload) override {}
};

struct SessionFixture {
  using Broker = axtp::BasicBroker<>;

  SessionFixture() : endpoint(broker) {}

  void start() {
    endpoint.attachTransport(transport);
    adapter =
        std::make_unique<axtp::WebSocketJsonRpcAdapter>(endpoint, transport);
    transport.bind(*adapter);
    transport.open();
  }

  Broker broker;
  axtp::AxtpEndpoint<Broker> endpoint;
  axtp::MockTransport transport;
  std::unique_ptr<axtp::WebSocketJsonRpcAdapter> adapter;
};

void injectJson(axtp::MockTransport &transport, const std::string &text) {
  transport.injectIncoming(axtp::Bytes(text.begin(), text.end()));
}

nlohmann::json popJson(axtp::MockTransport &transport, const char *label) {
  auto bytes = transport.tryPopOutgoing();
  if (!bytes.has_value()) {
    std::cerr << "missing JSON output: " << label << '\n';
    assert(bytes.has_value());
  }
  return nlohmann::json::parse(std::string(bytes->begin(), bytes->end()));
}

std::string jsonString(const nlohmann::json &object, const char *key) {
  return object.at(key).get<std::string>();
}

} // namespace

int main() {
  {
    SessionFixture fixture;
    fixture.broker.registerMethod(0x0901, [](const axtp::RpcPayload &) {
      const std::string result = R"({"ok":true})";
      return axtp::Bytes(result.begin(), result.end());
    });
    fixture.start();

    injectJson(
        fixture.transport,
        R"({"sid":"","op":7,"d":{"id":700,"method":"audio.getAlgorithmConfig"}})");
    auto response = popJson(fixture.transport, "request before identify");
    assert(response.at("op").get<int>() ==
           static_cast<int>(axtp::RpcOp::RequestResponse));
    assert(response.at("d").at("status").at("code").get<int>() ==
           static_cast<int>(axtp::ErrorCode::ControlOpenRequired));

    injectJson(fixture.transport,
               R"({"sid":"","op":2,"d":{"resumeSid":"legacy-session"}})");
    auto identified = popJson(fixture.transport, "legacy identify");
    assert(identified.at("op").get<int>() ==
           static_cast<int>(axtp::RpcOp::Identified));
    assert(identified.at("sid").get<std::string>() == "legacy-session");

    injectJson(
        fixture.transport,
        R"({"sid":"legacy-session","op":4,"d":{"eventMasks":"cast.*"}})");
    auto reidentified = popJson(fixture.transport, "reidentify");
    assert(reidentified.at("op").get<int>() ==
           static_cast<int>(axtp::RpcOp::Identified));
    assert(reidentified.at("sid").get<std::string>() == "legacy-session");

    injectJson(
        fixture.transport,
        R"({"sid":"","op":2,"d":{"randomSeed":7,"resumeSid":"replacement-session"}})");
    auto duplicate = popJson(fixture.transport, "duplicate identify");
    assert(duplicate.at("op").get<int>() ==
           static_cast<int>(axtp::RpcOp::Identified));
    assert(duplicate.at("sid").get<std::string>() == "legacy-session");

    injectJson(
        fixture.transport,
        R"({"sid":"legacy-session","op":7,"d":{"id":701,"method":"audio.getAlgorithmConfig","params":{}}})");
    response = popJson(fixture.transport, "legacy sid request");
    assert(response.at("sid").get<std::string>() == "legacy-session");
    assert(response.at("d").at("id").get<int>() == 701);
    assert(response.at("d").at("status").at("ok").get<bool>());
    assert(response.at("d").at("result").at("ok").get<bool>());
  }

  {
    SessionFixture fixture;
    fixture.start();
    injectJson(
        fixture.transport,
        R"({"sid":"","op":2,"d":{"randomSeed":305419896,"resumeSid":"resumed-session"}})");
    const auto identified =
        popJson(fixture.transport, "identify with resume sid");
    assert(identified.at("op").get<int>() ==
           static_cast<int>(axtp::RpcOp::Identified));
    assert(identified.at("sid").get<std::string>() == "resumed-session");
  }

  {
    SessionFixture fixture;
    fixture.broker.registerMethod(0x0901, [](const axtp::RpcPayload &request) {
      assert(request.meta.sourceProtocol == axtp::SourceProtocol::JsonRpc);
      assert(request.meta.jsonMethodOrEventName == "audio.getAlgorithmConfig");
      const std::string result =
          R"({"noiseSuppression":{"enabled":true,"level":3}})";
      return axtp::Bytes(result.begin(), result.end());
    });
    fixture.broker.registerMethod(0x0902, [](const axtp::RpcPayload &request) {
      const std::string params(request.body.begin(), request.body.end());
      assert(params == R"({"noiseSuppression":{"enabled":true,"level":3}})");
      return axtp::Bytes{};
    });
    fixture.broker.registerMethod(
        0x090D, [](const axtp::RpcPayload &) { return axtp::Bytes{0xB1}; });
    fixture.start();

    injectJson(
        fixture.transport,
        R"({"sid":"","op":2,"d":{"randomSeed":305419896,"eventMasks":"850101"}})");
    auto identified = popJson(fixture.transport, "identified");
    assert(identified.at("op").get<int>() ==
           static_cast<int>(axtp::RpcOp::Identified));
    const auto sid = jsonString(identified, "sid");
    assert(std::regex_match(sid, std::regex("^[0-9A-F]{8}$")));
    assert(sid != "00000000");
    assert(sid != "12345678");

    injectJson(
        fixture.transport,
        R"({"sid":")" + sid +
            R"(","op":7,"d":{"id":702,"method":"audio.getAlgorithmConfig","params":{}}})");
    auto response = popJson(fixture.transport, "audio.getAlgorithmConfig");
    assert(response.at("sid").get<std::string>() == sid);
    assert(response.at("op").get<int>() ==
           static_cast<int>(axtp::RpcOp::RequestResponse));
    auto data = response.at("d");
    assert(data.at("id").get<int>() == 702);
    assert(data.at("status").at("ok").get<bool>());
    assert(data.at("result").contains("noiseSuppression"));

    injectJson(
        fixture.transport,
        R"({"sid":")" + sid +
            R"(","op":7,"d":{"id":703,"method":"audio.setAlgorithmConfig","params":{"noiseSuppression":{"enabled":true,"level":3}}}})");
    response = popJson(fixture.transport, "audio.setAlgorithmConfig");
    data = response.at("d");
    assert(data.at("id").get<int>() == 703);
    assert(data.at("status").at("ok").get<bool>());
    assert(!data.contains("result"));

    injectJson(
        fixture.transport,
        R"({"sid":")" + sid +
            R"(","op":7,"d":{"id":704,"method":"audio.unknown","params":{}}})");
    response = popJson(fixture.transport, "unknown method");
    data = response.at("d");
    assert(data.at("status").at("code").get<int>() ==
           static_cast<int>(axtp::ErrorCode::RpcMethodNotFound));

    injectJson(
        fixture.transport,
        R"({"sid":")" + sid +
            R"(","op":7,"d":{"id":705,"method":"audio.getAlgorithmCapabilities","params":{}}})");
    response = popJson(fixture.transport, "invalid JSON response body");
    data = response.at("d");
    assert(data.at("status").at("code").get<int>() ==
           static_cast<int>(axtp::ErrorCode::RpcBodyDecodeFailed));
    assert(!data.contains("result"));

    injectJson(fixture.transport,
               R"({"sid":")" + sid +
                   R"(","op":9,"d":{"id":706,"requests":[]}})");
    response = popJson(fixture.transport, "batch unsupported");
    data = response.at("d");
    assert(response.at("op").get<int>() ==
           static_cast<int>(axtp::RpcOp::RequestBatchResponse));
    assert(data.at("status").at("code").get<int>() ==
           static_cast<int>(axtp::ErrorCode::RpcBatchUnsupported));

    injectJson(
        fixture.transport,
        R"({"sid":"0x000003","op":7,"d":{"id":707,"method":"audio.getAlgorithmConfig","params":{}}})");
    response = popJson(fixture.transport, "invalid sid");
    data = response.at("d");
    assert(response.at("op").get<int>() ==
           static_cast<int>(axtp::RpcOp::RequestResponse));
    assert(data.at("id").get<int>() == 707);
    assert(!data.at("status").at("ok").get<bool>());
    assert(data.at("status").at("code").get<int>() ==
           static_cast<int>(axtp::ErrorCode::RpcPayloadInvalid));

    axtp::RpcPayload event;
    event.encoding = axtp::RpcEncoding::Json;
    event.op = axtp::RpcOp::Event;
    event.methodOrEventId = 0x0901;
    event.bodyEncoding = axtp::RpcBodyEncoding::None;
    event.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
    const std::string body = R"({"state":"changed"})";
    event.body = axtp::Bytes(body.begin(), body.end());
    fixture.adapter->sendEvent(std::move(event));

    const auto emitted = popJson(fixture.transport, "event sid");
    assert(emitted.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Event));
    assert(emitted.at("sid").get<std::string>() == sid);
    assert(emitted.at("d").at("data").at("state").get<std::string>() ==
           "changed");

    axtp::RpcPayload explicitSidEvent;
    explicitSidEvent.encoding = axtp::RpcEncoding::Json;
    explicitSidEvent.op = axtp::RpcOp::Event;
    explicitSidEvent.methodOrEventId = 0x0901;
    explicitSidEvent.bodyEncoding = axtp::RpcBodyEncoding::None;
    explicitSidEvent.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
    explicitSidEvent.meta.jsonSid = sid;
    explicitSidEvent.body = axtp::Bytes(body.begin(), body.end());
    fixture.adapter->sendEvent(std::move(explicitSidEvent));

    const auto explicitEmission =
        popJson(fixture.transport, "event with explicit sid");
    assert(explicitEmission.at("sid").get<std::string>() == sid);
  }

  {
    CapturingPayloadSink sink;
    axtp::JsonRpcDecoder decoder(sink);
    const std::string text =
        R"({"sid":"json-session","op":7,"d":{"id":901,"method":"audio.getAlgorithmConfig","params":{}}})";
    decoder.onBytes(reinterpret_cast<const axtp::Byte *>(text.data()),
                    text.size());
    assert(sink.rpcs.size() == 1);
    assert(sink.rpcs[0].op == axtp::RpcOp::Request);
    assert(sink.rpcs[0].requestId == 901);
    assert(sink.rpcs[0].methodOrEventId == 0x0901);
    assert(sink.rpcs[0].meta.sourceProtocol == axtp::SourceProtocol::JsonRpc);
  }

  return 0;
}
