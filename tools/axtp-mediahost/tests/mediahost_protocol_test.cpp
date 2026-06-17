#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/inbound/inbound_processor.hpp"
#include "core/outbound/outbound_processor.hpp"
#include "io/byte_writer_sink.hpp"
#include "media_host.hpp"
#include "testing/mock_transport.hpp"

namespace {

struct CapturingByteWriter : axtp::IByteWriter {
    axtp::Bytes bytes;

    void writeBytes(const axtp::Byte* data, std::size_t size) override {
        bytes.insert(bytes.end(), data, data + size);
    }
};

struct CapturingPayloadSink : axtp::IPayloadSink {
    std::vector<axtp::RpcPayload> rpcs;
    std::vector<axtp::StreamPayload> streams;

    void onControl(axtp::ControlPayload) override {}

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

axtp::Bytes encodeStream(axtp::StreamPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendStream(std::move(payload));
    return writer.bytes;
}

axtp::RpcPayload makeJsonRequest(std::uint32_t requestId,
                                 axtp::MethodId methodId,
                                 std::string methodName,
                                 std::string body) {
    axtp::RpcPayload request;
    request.encoding = axtp::RpcEncoding::Json;
    request.op = axtp::RpcOp::Request;
    request.requestId = requestId;
    request.methodOrEventId = static_cast<std::uint16_t>(methodId);
    request.bodyEncoding = axtp::RpcBodyEncoding::None;
    request.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
    request.meta.jsonSid = "12345678";
    request.meta.jsonMethodOrEventName = std::move(methodName);
    request.body.assign(body.begin(), body.end());
    return request;
}

axtp::RpcPayload decodeSingleRpc(const axtp::Bytes& bytes) {
    CapturingPayloadSink sink;
    axtp::InboundProcessor inbound(sink);
    inbound.onBytes(bytes.data(), bytes.size());
    assert(sink.rpcs.size() == 1);
    return sink.rpcs.front();
}

}  // namespace

int main() {
    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        transport.open();

        std::uint32_t seenStreamId = 0;
        broker.registerStreamHandler(
            [&seenStreamId](const axtp::BrokerContext&, const axtp::StreamPayload& stream) {
                seenStreamId = stream.streamId;
            });

        axtp::StreamPayload stream;
        stream.streamId = 0x1001;
        stream.seqId = 0;
        stream.cursor = 10;
        stream.data = {0x00, 0x00, 0x01, 0x65};
        transport.injectIncoming(encodeStream(std::move(stream)));
        endpoint.poll(8);

        assert(seenStreamId == 0x1001);
        assert(!transport.tryPopOutgoing().has_value());
    }

    {
        const auto dumpDir =
            std::filesystem::temp_directory_path() / "axtp-mediahost-protocol-test";
        std::error_code ec;
        std::filesystem::remove_all(dumpDir, ec);

        axtp::mediahost::MediaHostOptions options;
        options.dumpDir = dumpDir;
        axtp::BasicBroker<> broker;
        axtp::mediahost::MediaStreamRegistry registry(options);
        axtp::mediahost::installMediaHostHandlers(broker, registry);

        axtp::AxtpEndpoint endpoint(broker);
        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        transport.open();

        const std::string openParams =
            R"({"source":"wireless_cast_video","peerRole":"receiver","codec":"h264"})";
        transport.injectIncoming(encodeRpc(makeJsonRequest(77,
                                                           axtp::MethodId::VideoOpenStream,
                                                           "video.openStream",
                                                           openParams)));
        endpoint.poll(8);
        auto outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        auto openResponse = decodeSingleRpc(*outgoing);
        assert(openResponse.op == axtp::RpcOp::RequestResponse);
        assert(openResponse.requestId == 77);
        assert(openResponse.statusCode == axtp::ErrorCode::Success);
        const std::string body(openResponse.body.begin(), openResponse.body.end());
        const auto parsed = nlohmann::json::parse(body);
        assert(parsed.at("streamId").get<std::uint32_t>() == 0x1001);
        assert(parsed.at("codec").get<std::string>() == "h264");
        assert(parsed.at("codecFormat").get<std::string>() == "annexb");

        axtp::StreamPayload stream;
        stream.streamId = 0x1001;
        stream.seqId = 0;
        stream.cursor = 1000;
        stream.data = {0x00, 0x00, 0x01, 0x67, 0x42};
        transport.injectIncoming(encodeStream(std::move(stream)));
        endpoint.poll(8);
        assert(!transport.tryPopOutgoing().has_value());
        auto stats = registry.stats();
        assert(stats.videoChunks == 1);
        assert(stats.videoBytes == 5);

        const std::string closeParams = R"({"streamId":4097,"peerRole":"transmitter"})";
        transport.injectIncoming(encodeRpc(makeJsonRequest(78,
                                                           axtp::MethodId::VideoCloseStream,
                                                           "video.closeStream",
                                                           closeParams)));
        endpoint.poll(8);
        outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        auto closeResponse = decodeSingleRpc(*outgoing);
        assert(closeResponse.statusCode == axtp::ErrorCode::Success);

        const auto dumpPath = dumpDir / "video-0x00001001.h264";
        assert(std::filesystem::exists(dumpPath));
        assert(std::filesystem::file_size(dumpPath) == 5);

        axtp::StreamPayload late;
        late.streamId = 0x1001;
        late.seqId = 1;
        late.cursor = 2000;
        late.data = {0x00};
        transport.injectIncoming(encodeStream(std::move(late)));
        endpoint.poll(8);
        stats = registry.stats();
        assert(stats.unknownChunks == 1);
    }

    return 0;
}
