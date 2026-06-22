#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/protocol/wire/inbound_processor.hpp"
#include "core/protocol/wire/outbound_processor.hpp"
#include "core/support/io/byte_writer_sink.hpp"
#include "profiles/media/media_host.hpp"
#include "core/runtime/testing/mock_transport.hpp"

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

struct CountingMediaSink : axtp::mediahost::IMediaStreamSink {
    std::vector<axtp::mediahost::MediaStreamInfo> opened;
    std::vector<axtp::StreamPayload> chunks;
    std::vector<std::uint32_t> closed;

    void onStreamOpened(const axtp::mediahost::MediaStreamInfo& info) override {
        opened.push_back(info);
    }

    void onStreamChunk(axtp::mediahost::MediaKind, const axtp::StreamPayload& stream) override {
        chunks.push_back(stream);
    }

    void onStreamClosed(axtp::mediahost::MediaKind, std::uint32_t streamId) override {
        closed.push_back(streamId);
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

axtp::RpcPayload
makeJsonResponse(std::uint32_t requestId, axtp::ErrorCode status, std::string body) {
    axtp::RpcPayload response;
    response.encoding = axtp::RpcEncoding::Json;
    response.op = axtp::RpcOp::RequestResponse;
    response.requestId = requestId;
    response.statusCode = status;
    response.bodyEncoding = axtp::RpcBodyEncoding::None;
    response.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
    response.meta.jsonSid = "12345678";
    response.body.assign(body.begin(), body.end());
    return response;
}

axtp::RpcPayload makeJsonEvent(axtp::EventId eventId, std::string eventName, std::string body) {
    axtp::RpcPayload event;
    event.encoding = axtp::RpcEncoding::Json;
    event.op = axtp::RpcOp::Event;
    event.methodOrEventId = static_cast<std::uint16_t>(eventId);
    event.bodyEncoding = axtp::RpcBodyEncoding::None;
    event.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
    event.meta.jsonSid = "12345678";
    event.meta.jsonMethodOrEventName = std::move(eventName);
    event.body.assign(body.begin(), body.end());
    return event;
}

axtp::RpcPayload decodeSingleRpc(const axtp::Bytes& bytes) {
    CapturingPayloadSink sink;
    axtp::InboundProcessor inbound(sink);
    inbound.onBytes(bytes.data(), bytes.size());
    assert(sink.rpcs.size() == 1);
    return sink.rpcs.front();
}

} // namespace

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
        options.openMode = axtp::mediahost::OpenMode::ProducerOpen;
        CountingMediaSink mediaSink;
        options.streamSink = &mediaSink;
        axtp::BasicBroker<> broker;
        axtp::mediahost::MediaStreamRegistry registry(options);
        axtp::mediahost::installMediaHostHandlers(broker, registry);

        axtp::AxtpEndpoint endpoint(broker);
        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        transport.open();

        const std::string openParams =
            R"({"source":"wireless_cast_video","peerRole":"receiver","codec":"h264"})";
        transport.injectIncoming(encodeRpc(
            makeJsonRequest(77, axtp::MethodId::VideoOpenStream, "video.openStream", openParams)));
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
        assert(mediaSink.opened.size() == 1);
        assert(mediaSink.opened.front().streamId == 0x1001);
        assert(mediaSink.opened.front().kind == axtp::mediahost::MediaKind::Video);

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
        assert(mediaSink.chunks.size() == 1);
        assert(mediaSink.chunks.front().streamId == 0x1001);

        const std::string closeParams = R"({"streamId":4097,"peerRole":"transmitter"})";
        transport.injectIncoming(encodeRpc(makeJsonRequest(
            78, axtp::MethodId::VideoCloseStream, "video.closeStream", closeParams)));
        endpoint.poll(8);
        outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        auto closeResponse = decodeSingleRpc(*outgoing);
        assert(closeResponse.statusCode == axtp::ErrorCode::Success);
        assert(mediaSink.closed.size() == 1);
        assert(mediaSink.closed.front() == 0x1001);

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
        assert(mediaSink.chunks.size() == 1);
    }

    {
        axtp::mediahost::MediaHostOptions options;
        options.openMode = axtp::mediahost::OpenMode::ReceiverPull;
        axtp::BasicBroker<> broker;
        axtp::mediahost::MediaStreamRegistry registry(options);
        axtp::mediahost::MediaPullCoordinator pullCoordinator(
            registry, "12345678", std::chrono::milliseconds(1000));
        axtp::mediahost::installMediaHostHandlers(broker, registry);
        broker.registerEventHandler(
            [&pullCoordinator](const axtp::BrokerContext&, const axtp::RpcPayload& event) {
                pullCoordinator.handleEvent(event);
            });

        axtp::AxtpEndpoint endpoint(broker);
        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        transport.open();

        const std::string videoEvent = R"({"source":"wireless_cast_video","state":"receiving"})";
        transport.injectIncoming(
            encodeRpc(makeJsonEvent(axtp::EventId::VideoStreamSourceStateChanged,
                                    "video.streamSourceStateChanged",
                                    videoEvent)));
        endpoint.poll(8);
        pullCoordinator.poll(endpoint);
        auto outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        auto videoOpen = decodeSingleRpc(*outgoing);
        assert(videoOpen.op == axtp::RpcOp::Request);
        assert(videoOpen.methodOrEventId ==
               static_cast<std::uint16_t>(axtp::MethodId::VideoOpenStream));
        const auto videoParams =
            nlohmann::json::parse(std::string(videoOpen.body.begin(), videoOpen.body.end()));
        assert(videoParams.at("source").get<std::string>() == "wireless_cast_video");
        assert(videoParams.at("peerRole").get<std::string>() == "transmitter");
        assert(videoParams.at("codec").get<std::string>() == "h264");

        transport.injectIncoming(
            encodeRpc(makeJsonEvent(axtp::EventId::VideoStreamSourceStateChanged,
                                    "video.streamSourceStateChanged",
                                    videoEvent)));
        endpoint.poll(8);
        pullCoordinator.poll(endpoint);
        assert(!transport.tryPopOutgoing().has_value());

        const std::string videoResult =
            R"({"streamId":13107,"state":"streaming","source":"wireless_cast_video","peerRole":"transmitter","codec":"h264","streamProfile":"media.video","cursorUnit":"timestampUs"})";
        transport.injectIncoming(encodeRpc(
            makeJsonResponse(videoOpen.requestId, axtp::ErrorCode::Success, videoResult)));
        endpoint.poll(8);
        pullCoordinator.poll(endpoint);
        assert(!transport.tryPopOutgoing().has_value());

        axtp::StreamPayload videoStream;
        videoStream.streamId = 13107;
        videoStream.seqId = 0;
        videoStream.cursor = 3000;
        videoStream.data = {0x00, 0x00, 0x01, 0x65};
        transport.injectIncoming(encodeStream(std::move(videoStream)));
        endpoint.poll(8);
        auto stats = registry.stats();
        assert(stats.videoChunks == 1);
        assert(stats.videoBytes == 4);

        const std::string audioEvent = R"({"source":"wireless_cast_audio","state":"available"})";
        transport.injectIncoming(
            encodeRpc(makeJsonEvent(axtp::EventId::AudioStreamSourceStateChanged,
                                    "audio.streamSourceStateChanged",
                                    audioEvent)));
        endpoint.poll(8);
        pullCoordinator.poll(endpoint);
        outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        auto audioOpen = decodeSingleRpc(*outgoing);
        assert(audioOpen.op == axtp::RpcOp::Request);
        assert(audioOpen.methodOrEventId ==
               static_cast<std::uint16_t>(axtp::MethodId::AudioOpenStream));
        const auto audioParams =
            nlohmann::json::parse(std::string(audioOpen.body.begin(), audioOpen.body.end()));
        assert(audioParams.at("source").get<std::string>() == "wireless_cast_audio");
        assert(audioParams.at("peerRole").get<std::string>() == "transmitter");
        assert(audioParams.at("codec").get<std::string>() == "aac");
        assert(audioParams.at("transportFormat").get<std::string>() == "adts");
        assert(audioParams.at("sampleRate").get<std::uint32_t>() == 48000);
        assert(audioParams.at("channels").get<std::uint32_t>() == 1);

        const std::string audioResult =
            R"({"streamId":17476,"state":"streaming","source":"wireless_cast_audio","peerRole":"transmitter","codec":"aac","transportFormat":"adts","sampleRate":48000,"channels":2,"streamProfile":"media.audio","cursorUnit":"timestampUs"})";
        transport.injectIncoming(encodeRpc(
            makeJsonResponse(audioOpen.requestId, axtp::ErrorCode::Success, audioResult)));
        endpoint.poll(8);
        pullCoordinator.poll(endpoint);

        axtp::StreamPayload audioStream;
        audioStream.streamId = 17476;
        audioStream.seqId = 0;
        audioStream.cursor = 3000;
        audioStream.data = {0xFF, 0xF1, 0x50};
        transport.injectIncoming(encodeStream(std::move(audioStream)));
        endpoint.poll(8);
        stats = registry.stats();
        assert(stats.audioChunks == 1);
        assert(stats.audioBytes == 3);
    }

    {
        axtp::mediahost::MediaHostOptions options;
        options.openMode = axtp::mediahost::OpenMode::ReceiverPull;
        axtp::BasicBroker<> broker;
        axtp::mediahost::MediaStreamRegistry registry(options);
        axtp::mediahost::MediaPullCoordinator pullCoordinator(
            registry, "12345678", std::chrono::milliseconds(1000));
        axtp::mediahost::installMediaHostHandlers(broker, registry);
        broker.registerEventHandler(
            [&pullCoordinator](const axtp::BrokerContext&, const axtp::RpcPayload& event) {
                pullCoordinator.handleEvent(event);
            });

        axtp::AxtpEndpoint endpoint(broker);
        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        transport.open();

        transport.injectIncoming(
            encodeRpc(makeJsonEvent(axtp::EventId::VideoStreamSourceStateChanged,
                                    "video.streamSourceStateChanged",
                                    R"({"source":"wireless_cast","state":"receiving"})")));
        transport.injectIncoming(
            encodeRpc(makeJsonEvent(axtp::EventId::AudioStreamSourceStateChanged,
                                    "audio.streamSourceStateChanged",
                                    R"({"source":"wireless_cast_audio","state":"receiving"})")));
        endpoint.poll(8);
        pullCoordinator.poll(endpoint);
        auto outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        auto videoOpen = decodeSingleRpc(*outgoing);
        assert(videoOpen.methodOrEventId ==
               static_cast<std::uint16_t>(axtp::MethodId::VideoOpenStream));
        assert(!transport.tryPopOutgoing().has_value());

        const std::string videoResult =
            R"({"streamId":13107,"state":"streaming","source":"wireless_cast","peerRole":"transmitter","codec":"h264","streamProfile":"media.video","cursorUnit":"timestampUs"})";
        transport.injectIncoming(encodeRpc(
            makeJsonResponse(videoOpen.requestId, axtp::ErrorCode::Success, videoResult)));
        endpoint.poll(8);
        pullCoordinator.poll(endpoint);
        outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        auto audioOpen = decodeSingleRpc(*outgoing);
        assert(audioOpen.methodOrEventId ==
               static_cast<std::uint16_t>(axtp::MethodId::AudioOpenStream));
    }

    {
        axtp::mediahost::MediaHostOptions options;
        options.acceptAudio = false;
        options.openMode = axtp::mediahost::OpenMode::ReceiverPull;
        axtp::BasicBroker<> broker;
        axtp::mediahost::MediaStreamRegistry registry(options);
        axtp::mediahost::MediaPullCoordinator pullCoordinator(
            registry, "12345678", std::chrono::milliseconds(1000));
        broker.registerEventHandler(
            [&pullCoordinator](const axtp::BrokerContext&, const axtp::RpcPayload& event) {
                pullCoordinator.handleEvent(event);
            });

        axtp::AxtpEndpoint endpoint(broker);
        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        transport.open();

        transport.injectIncoming(
            encodeRpc(makeJsonEvent(axtp::EventId::AudioStreamSourceStateChanged,
                                    "audio.streamSourceStateChanged",
                                    R"({"source":"wireless_cast_audio","state":"receiving"})")));
        endpoint.poll(8);
        pullCoordinator.poll(endpoint);
        assert(!transport.tryPopOutgoing().has_value());
    }

    {
        axtp::mediahost::MediaHostOptions options;
        options.openMode = axtp::mediahost::OpenMode::ReceiverPull;
        axtp::BasicBroker<> broker;
        axtp::mediahost::MediaStreamRegistry registry(options);
        axtp::mediahost::installMediaHostHandlers(broker, registry);

        axtp::AxtpEndpoint endpoint(broker);
        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        transport.open();

        const std::string openParams =
            R"({"source":"wireless_cast_video","peerRole":"receiver","codec":"h264"})";
        transport.injectIncoming(encodeRpc(
            makeJsonRequest(79, axtp::MethodId::VideoOpenStream, "video.openStream", openParams)));
        endpoint.poll(8);
        const auto outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        const auto response = decodeSingleRpc(*outgoing);
        assert(response.op == axtp::RpcOp::RequestResponse);
        assert(response.requestId == 79);
        assert(response.statusCode == axtp::ErrorCode::RpcParamInvalid);
    }

    {
        axtp::mediahost::MediaHostOptions options;
        options.openMode = axtp::mediahost::OpenMode::Both;
        axtp::BasicBroker<> broker;
        axtp::mediahost::MediaStreamRegistry registry(options);
        axtp::mediahost::MediaPullCoordinator pullCoordinator(
            registry, "12345678", std::chrono::milliseconds(1000));
        axtp::mediahost::installMediaHostHandlers(broker, registry);
        broker.registerEventHandler(
            [&pullCoordinator](const axtp::BrokerContext&, const axtp::RpcPayload& event) {
                pullCoordinator.handleEvent(event);
            });

        axtp::AxtpEndpoint endpoint(broker);
        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        transport.open();

        const std::string openParams =
            R"({"source":"wireless_cast_video","peerRole":"receiver","codec":"h264"})";
        transport.injectIncoming(encodeRpc(
            makeJsonRequest(80, axtp::MethodId::VideoOpenStream, "video.openStream", openParams)));
        endpoint.poll(8);
        auto outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        const auto response = decodeSingleRpc(*outgoing);
        assert(response.statusCode == axtp::ErrorCode::Success);

        transport.injectIncoming(
            encodeRpc(makeJsonEvent(axtp::EventId::VideoStreamSourceStateChanged,
                                    "video.streamSourceStateChanged",
                                    R"({"source":"wireless_cast_video","state":"receiving"})")));
        endpoint.poll(8);
        pullCoordinator.poll(endpoint);
        assert(!transport.tryPopOutgoing().has_value());
    }

    {
        axtp::mediahost::MediaHostOptions options;
        options.openMode = axtp::mediahost::OpenMode::ProducerOpen;
        CountingMediaSink mediaSink;
        options.streamSink = &mediaSink;
        axtp::mediahost::MediaStreamRegistry registry(options);

        const auto opened = registry.acceptProducerOpen(
            axtp::mediahost::MediaKind::Video,
            R"({"source":"wireless_cast_video","peerRole":"receiver","codec":"h264"})");
        assert(opened.status == axtp::ErrorCode::Success);
        auto snapshot = registry.activeStreamsSnapshot();
        assert(snapshot.size() == 1);
        assert(snapshot.front().kind == axtp::mediahost::MediaKind::Video);
        assert(snapshot.front().streamId == 0x1001);
        assert(snapshot.front().source == "wireless_cast_video");

        const auto closed =
            registry.closeLocal(snapshot.front().kind, snapshot.front().streamId);
        assert(closed.status == axtp::ErrorCode::Success);
        assert(registry.activeStreamCount() == 0);
        assert(mediaSink.closed.size() == 1);
        assert(mediaSink.closed.front() == 0x1001);
    }

    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        transport.open();

        axtp::mediahost::MediaCloseCoordinator closeCoordinator(
            "12345678", std::chrono::milliseconds(1000));
        closeCoordinator.sendClose(endpoint,
                                   axtp::mediahost::ActiveMediaStream{
                                       axtp::mediahost::MediaKind::Video,
                                       0x3333,
                                       "wireless_cast_video"});
        auto outgoing = transport.tryPopOutgoing();
        assert(outgoing.has_value());
        auto closeRequest = decodeSingleRpc(*outgoing);
        assert(closeRequest.op == axtp::RpcOp::Request);
        assert(closeRequest.methodOrEventId ==
               static_cast<std::uint16_t>(axtp::MethodId::VideoCloseStream));
        assert(closeRequest.meta.jsonSid == "12345678");
        const auto params =
            nlohmann::json::parse(std::string(closeRequest.body.begin(), closeRequest.body.end()));
        assert(params.at("streamId").get<std::uint32_t>() == 0x3333);
        assert(params.at("peerRole").get<std::string>() == "transmitter");
        assert(closeCoordinator.pendingCount() == 1);

        transport.injectIncoming(encodeRpc(
            makeJsonResponse(closeRequest.requestId,
                             axtp::ErrorCode::Success,
                             R"({"streamId":13107,"state":"closed"})")));
        endpoint.poll(8);
        closeCoordinator.poll(endpoint);
        assert(closeCoordinator.pendingCount() == 0);
    }

    return 0;
}
