#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/protocol/wire/inbound_processor.hpp"
#include "core/runtime/testing/mock_transport.hpp"
#include "profiles/firmware/firmware_profile.hpp"

namespace {

struct CapturingPayloadSink : axtp::IPayloadSink {
    std::vector<axtp::StreamPayload> streams;

    void onControl(axtp::ControlPayload) override {}
    void onRpc(axtp::RpcPayload) override {}

    void onStream(axtp::StreamPayload payload) override {
        streams.push_back(std::move(payload));
    }
};

axtp::StreamPayload decodeSingleStream(const axtp::Bytes& bytes) {
    CapturingPayloadSink sink;
    axtp::InboundProcessor inbound(sink);
    inbound.onBytes(bytes.data(), bytes.size());
    assert(sink.streams.size() == 1);
    return sink.streams.front();
}

axtp::Bytes jsonBytes(const nlohmann::json& object) {
    const auto body = object.dump();
    return axtp::Bytes(body.begin(), body.end());
}

} // namespace

int main() {
    axtp::sdk::AxtpClient client;
    auto transport = std::make_unique<axtp::MockTransport>();
    auto* transportPtr = transport.get();
    client.attachTransport(std::move(transport));

    nlohmann::json beginParams;
    nlohmann::json finishParams;
    client.registerMethod(
        static_cast<std::uint16_t>(axtp::MethodId::FirmwareBeginUpdate),
        [&beginParams](const axtp::RpcPayload& request) {
            beginParams = nlohmann::json::parse(
                std::string(request.body.begin(), request.body.end()));
            return jsonBytes({
                {"updateSessionId", "update-1"},
                {"state", "receiving"},
                {"streams", nlohmann::json::array({{{"fileId", "app"}, {"streamId", 0x1001}}})},
                {"chunkSize", 2},
            });
        });
    client.registerMethod(
        static_cast<std::uint16_t>(axtp::MethodId::FirmwareFinishUpdate),
        [&finishParams](const axtp::RpcPayload& request) {
            finishParams = nlohmann::json::parse(
                std::string(request.body.begin(), request.body.end()));
            return jsonBytes({
                {"updateSessionId", "update-1"},
                {"accepted", true},
                {"state", "verifying"},
            });
        });

    axtp::firmware::FirmwareUpdateProfile profile(client);
    axtp::firmware::FirmwareUpdateRequest request;
    request.file.fileId = "app";
    request.file.target = "primary";
    request.file.data = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4};
    request.file.md5 = "00112233445566778899aabbccddeeff";
    request.packageId = "pkg-1";
    request.version = "1.2.3";
    request.preferredChunkSize = 4;

    axtp::sdk::CallOptions callOptions;
    callOptions.timeout = std::chrono::milliseconds(100);
    const auto result = profile.update(request, callOptions);

    assert(result.ok);
    assert(result.status == axtp::ErrorCode::Success);
    assert(result.updateSessionId == "update-1");
    assert(result.streamId == 0x1001);
    assert(result.chunkSize == 2);
    assert(result.chunks == 3);
    assert(result.bytes == 5);
    assert(result.finish.at("accepted").get<bool>());

    assert(beginParams.at("manifest").at("packageId").get<std::string>() == "pkg-1");
    assert(beginParams.at("manifest").at("version").get<std::string>() == "1.2.3");
    const auto manifestFile = beginParams.at("manifest").at("files").front();
    assert(manifestFile.at("fileId").get<std::string>() == "app");
    assert(manifestFile.at("target").get<std::string>() == "primary");
    assert(manifestFile.at("size").get<std::uint64_t>() == 5);
    assert(manifestFile.at("md5").get<std::string>() == "00112233445566778899aabbccddeeff");

    auto outgoing = transportPtr->tryPopOutgoing();
    assert(outgoing.has_value());
    auto stream = decodeSingleStream(*outgoing);
    assert(stream.streamId == 0x1001);
    assert(stream.seqId == 0);
    assert(stream.cursor == 0);
    assert((stream.data == axtp::Bytes{0xA0, 0xA1}));

    outgoing = transportPtr->tryPopOutgoing();
    assert(outgoing.has_value());
    stream = decodeSingleStream(*outgoing);
    assert(stream.seqId == 1);
    assert(stream.cursor == 2);
    assert((stream.data == axtp::Bytes{0xA2, 0xA3}));

    outgoing = transportPtr->tryPopOutgoing();
    assert(outgoing.has_value());
    stream = decodeSingleStream(*outgoing);
    assert(stream.seqId == 2);
    assert(stream.cursor == 4);
    assert((stream.data == axtp::Bytes{0xA4}));
    assert(!transportPtr->tryPopOutgoing().has_value());

    assert(finishParams.at("updateSessionId").get<std::string>() == "update-1");
}
