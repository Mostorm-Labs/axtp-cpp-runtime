#include <cassert>
#include <memory>
#include <string>

#include "testing/mock_transport.hpp"

#include "axtp_sdk_all.hpp"

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
    raw.bodyEncoding = axtp::RpcBodyEncoding::RawBytes;
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
}
