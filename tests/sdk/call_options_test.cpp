#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <axtp_sdk.hpp>
#include <core/protocol/session/pending_call_table.hpp>
#include <core/protocol/wire/inbound_processor.hpp>
#include <core/protocol/wire/outbound_processor.hpp>
#include <core/runtime/testing/mock_transport.hpp>

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

    void onControl(axtp::ControlPayload payload) override {
        controls.push_back(std::move(payload));
    }

    void onRpc(axtp::RpcPayload payload) override {
        rpcs.push_back(std::move(payload));
    }

    void onStream(axtp::StreamPayload) override {}
};

class JsonMockTransport final : public axtp::ITransport {
public:
    void bind(axtp::IByteSink& sink) override { _sink = &sink; }
    void open() override { _open = true; }
    void close() override { _open = false; }
    void poll() override {}
    void sendBytes(const axtp::Byte* data, std::size_t size) override {
        _outgoing.emplace_back(data, data + size);
    }
    axtp::TransportProfile profile() const override { return _profile; }

private:
    axtp::IByteSink* _sink = nullptr;
    bool _open = false;
    std::vector<axtp::Bytes> _outgoing;
    axtp::TransportProfile _profile{
        axtp::TransportKind::WebSocket,
        axtp::AxtpWireMode::WebSocketJsonRpc,
        axtp::RpcEncoding::Json,
        true,
        true,
        false,
        4096,
    };
};

class ThrowingPollTransport final : public axtp::MockTransport {
public:
    void poll() override {
        throw std::runtime_error("transport poll failed");
    }
};

axtp::Bytes encodeRpc(axtp::RpcPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendRpc(std::move(payload));
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

axtp::RpcPayload takeOutgoingRequest(axtp::MockTransport& transport) {
    const auto outgoing = transport.tryPopOutgoing();
    REQUIRE(outgoing.has_value());

    CapturingPayloadSink sink;
    axtp::InboundProcessor inbound(sink);
    inbound.onBytes(outgoing->data(), outgoing->size());
    REQUIRE(sink.rpcs.size() == 1);
    REQUIRE(sink.rpcs.front().op == axtp::RpcOp::Request);
    return std::move(sink.rpcs.front());
}

CapturingPayloadSink takeOutgoingPayloads(axtp::MockTransport& transport) {
    const auto outgoing = transport.tryPopOutgoing();
    REQUIRE(outgoing.has_value());

    CapturingPayloadSink sink;
    axtp::InboundProcessor inbound(sink);
    inbound.onBytes(outgoing->data(), outgoing->size());
    return sink;
}

std::optional<CapturingPayloadSink> tryTakeOutgoingPayloads(
    axtp::MockTransport& transport) {
    const auto outgoing = transport.tryPopOutgoing();
    if (!outgoing.has_value()) {
        return std::nullopt;
    }

    CapturingPayloadSink sink;
    axtp::InboundProcessor inbound(sink);
    inbound.onBytes(outgoing->data(), outgoing->size());
    return sink;
}

axtp::RpcPayload makeRequest(std::uint32_t methodId) {
    axtp::RpcPayload request;
    request.encoding = axtp::RpcEncoding::Json;
    request.op = axtp::RpcOp::Request;
    request.methodOrEventId = methodId;
    request.bodyEncoding = axtp::RpcBodyEncoding::None;
    request.meta.sourceProtocol = axtp::SourceProtocol::AxtpV1;
    request.body = {'{', '}'};
    return request;
}

axtp::RpcPayload makeResponse(const axtp::RpcPayload& request) {
    axtp::RpcPayload response;
    response.encoding = request.encoding;
    response.op = axtp::RpcOp::RequestResponse;
    response.requestId = request.requestId;
    response.methodOrEventId = request.methodOrEventId;
    response.statusCode = axtp::ErrorCode::Success;
    response.bodyEncoding = request.bodyEncoding;
    response.meta = request.meta;
    response.body = {'{', '}'};
    return response;
}

axtp::ControlPayload makeAccept(const axtp::ControlPayload& open) {
    axtp::ControlPayload accept;
    accept.opcode = axtp::ControlOpcode::Accept;
    accept.controlId = open.controlId;
    accept.statusCode = axtp::ErrorCode::Success;
    accept.tlv = axtp::ControlTlvCodec::defaultsForAccept(open.tlv);
    return accept;
}

}  // namespace

int main() {
    [[maybe_unused]] const axtp::sdk::CallOptions legacyAggregateOptions{
        std::chrono::milliseconds{100},
        axtp::RpcEncoding::Json,
        false,
        false,
    };

    axtp::sdk::ClientOptions clientOptions;
    clientOptions.autoIdentify = false;

    // An explicit id is only retired after its request is eligible to reach
    // the peer.  A failed pre-send attempt must not prevent a caller from
    // reusing its own correlation id after attaching a transport.
    axtp::sdk::AxtpClient preSendClient(clientOptions);
    auto preSendRequest = makeRequest(0x90010000);
    preSendRequest.requestId = 0x7000;
    preSendRequest.meta.requestId = preSendRequest.requestId;
    const auto unavailablePreSend = preSendClient.callRaw(preSendRequest);
    REQUIRE(unavailablePreSend.statusCode == axtp::ErrorCode::Unavailable);
    auto preSendTransport = std::make_unique<axtp::MockTransport>();
    auto* preSendTransportPtr = preSendTransport.get();
    preSendClient.attachTransport(std::move(preSendTransport));
    axtp::sdk::CallOptions retryPreSend;
    retryPreSend.timeout = std::chrono::milliseconds{100};
    retryPreSend.progress = [&] {
        const auto retryRequest = takeOutgoingRequest(*preSendTransportPtr);
        REQUIRE(retryRequest.requestId == preSendRequest.requestId);
    };
    retryPreSend.cancelled = [] { return true; };
    const auto retriedPreSend = preSendClient.callRaw(preSendRequest, retryPreSend);
    REQUIRE(retriedPreSend.statusCode == axtp::ErrorCode::Canceled);

    axtp::sdk::AxtpClient client(clientOptions);
    auto transport = std::make_unique<axtp::MockTransport>();
    auto* transportPtr = transport.get();
    client.attachTransport(std::move(transport));

    std::vector<axtp::StreamPayload> receivedStreams;
    client.setStreamHandler(
        [&receivedStreams](const axtp::BrokerContext&, const axtp::StreamPayload& stream) {
            receivedStreams.push_back(stream);
        });

    axtp::StreamPayload stream;
    stream.streamId = 0x1001;
    stream.seqId = 1;
    stream.cursor = 1000;
    stream.data = {0x01};
    transportPtr->injectIncoming(encodeStream(stream));

    std::size_t progressCalls = 0;
    std::size_t cancellationChecks = 0;
    bool progressRanAfterPoll = false;
    axtp::sdk::CallOptions responseWinsOptions;
    responseWinsOptions.timeout = std::chrono::milliseconds{0};
    responseWinsOptions.progress = [&] {
        ++progressCalls;
        progressRanAfterPoll = receivedStreams.size() == 1;
        const auto request = takeOutgoingRequest(*transportPtr);
        transportPtr->injectIncoming(encodeRpc(makeResponse(request)));
    };
    responseWinsOptions.cancelled = [&] {
        ++cancellationChecks;
        return true;
    };

    const auto responseWins =
        client.callRaw(makeRequest(0x90010001), responseWinsOptions);
    REQUIRE(responseWins.statusCode == axtp::ErrorCode::Success);
    REQUIRE(progressCalls == 1);
    REQUIRE(progressRanAfterPoll);
    REQUIRE(cancellationChecks == 0);

    std::optional<axtp::RpcPayload> cancelledRequest;
    axtp::sdk::CallOptions cancelOptions;
    cancelOptions.timeout = std::chrono::milliseconds{100};
    cancelOptions.progress = [&] {
        cancelledRequest = takeOutgoingRequest(*transportPtr);
    };
    cancelOptions.cancelled = [] {
        return true;
    };

    const auto cancelled = client.callRaw(makeRequest(0x90010002), cancelOptions);
    REQUIRE(cancelled.statusCode == axtp::ErrorCode::Canceled);
    REQUIRE(cancelledRequest.has_value());
    REQUIRE(cancelled.requestId == cancelledRequest->requestId);

    transportPtr->injectIncoming(encodeRpc(makeResponse(*cancelledRequest)));

    axtp::sdk::CallOptions cancelProbeOptions;
    cancelProbeOptions.timeout = std::chrono::milliseconds{100};
    cancelProbeOptions.acceptAnyResponse = true;
    cancelProbeOptions.progress = [&] {
        (void)takeOutgoingRequest(*transportPtr);
    };
    cancelProbeOptions.cancelled = [] {
        return true;
    };

    const auto cancelProbe =
        client.callRaw(makeRequest(0x90010003), cancelProbeOptions);
    REQUIRE(cancelProbe.statusCode == axtp::ErrorCode::Canceled);
    REQUIRE(cancelProbe.requestId != cancelledRequest->requestId);

    std::optional<axtp::RpcPayload> timedOutRequest;
    progressCalls = 0;
    axtp::sdk::CallOptions timeoutOptions;
    timeoutOptions.timeout = std::chrono::milliseconds{0};
    timeoutOptions.progress = [&] {
        ++progressCalls;
        timedOutRequest = takeOutgoingRequest(*transportPtr);
    };

    const auto timedOut = client.callRaw(makeRequest(0x90010004), timeoutOptions);
    REQUIRE(timedOut.statusCode == axtp::ErrorCode::RpcResponseTimeout);
    REQUIRE(progressCalls == 1);
    REQUIRE(timedOutRequest.has_value());
    REQUIRE(timedOut.requestId == timedOutRequest->requestId);

    transportPtr->injectIncoming(encodeRpc(makeResponse(*timedOutRequest)));

    const auto timeoutProbe =
        client.callRaw(makeRequest(0x90010005), cancelProbeOptions);
    REQUIRE(timeoutProbe.statusCode == axtp::ErrorCode::Canceled);
    REQUIRE(timeoutProbe.requestId != timedOutRequest->requestId);

    axtp::sdk::CallOptions finalOptions;
    finalOptions.timeout = std::chrono::milliseconds{100};
    finalOptions.progress = [&] {
        const auto request = takeOutgoingRequest(*transportPtr);
        transportPtr->injectIncoming(encodeRpc(makeResponse(request)));
    };

    const auto finalResponse = client.callRaw(makeRequest(0x90010006), finalOptions);
    REQUIRE(finalResponse.statusCode == axtp::ErrorCode::Success);

    // User callbacks are an embedding boundary: exceptions must not escape
    // the synchronous SDK call or leave a request id eligible for reuse.
    std::optional<axtp::RpcPayload> progressExceptionRequest;
    axtp::sdk::CallOptions progressExceptionOptions;
    progressExceptionOptions.timeout = std::chrono::milliseconds{100};
    progressExceptionOptions.progress = [&] {
        progressExceptionRequest = takeOutgoingRequest(*transportPtr);
        throw std::runtime_error("progress failed");
    };
    const auto progressException =
        client.callRaw(makeRequest(0x9001000C), progressExceptionOptions);
    REQUIRE(progressException.statusCode == axtp::ErrorCode::InternalError);
    REQUIRE(progressExceptionRequest.has_value());
    transportPtr->injectIncoming(encodeRpc(makeResponse(*progressExceptionRequest)));

    std::optional<axtp::RpcPayload> cancellationExceptionRequest;
    axtp::sdk::CallOptions cancellationExceptionOptions;
    cancellationExceptionOptions.timeout = std::chrono::milliseconds{100};
    cancellationExceptionOptions.progress = [&] {
        cancellationExceptionRequest = takeOutgoingRequest(*transportPtr);
    };
    cancellationExceptionOptions.cancelled = []() -> bool {
        throw std::runtime_error("cancel probe failed");
    };
    const auto cancellationException =
        client.callRaw(makeRequest(0x9001000D), cancellationExceptionOptions);
    REQUIRE(cancellationException.statusCode == axtp::ErrorCode::InternalError);
    REQUIRE(cancellationExceptionRequest.has_value());
    transportPtr->injectIncoming(encodeRpc(makeResponse(*cancellationExceptionRequest)));

    const auto afterCallbackExceptions =
        client.callRaw(makeRequest(0x9001000E), finalOptions);
    REQUIRE(afterCallbackExceptions.statusCode == axtp::ErrorCode::Success);

    // endpoint.poll() also invokes registered stream/event handlers.  Their
    // exceptions are embedding failures, not exceptions from callRaw(), and
    // the just-sent request must be retired before a late reply arrives.
    axtp::sdk::ClientOptions endpointCallbackClientOptions;
    endpointCallbackClientOptions.autoIdentify = false;
    axtp::sdk::AxtpClient endpointCallbackClient(endpointCallbackClientOptions);
    auto endpointCallbackTransport = std::make_unique<axtp::MockTransport>();
    auto* endpointCallbackTransportPtr = endpointCallbackTransport.get();
    endpointCallbackClient.attachTransport(std::move(endpointCallbackTransport));
    endpointCallbackClient.setStreamHandler(
        [](const axtp::BrokerContext&, const axtp::StreamPayload&) {
            throw std::runtime_error("stream handler failed");
        });

    axtp::StreamPayload throwingStream;
    throwingStream.streamId = 0x3001;
    throwingStream.seqId = 1;
    throwingStream.data = {0x01};
    endpointCallbackTransportPtr->injectIncoming(encodeStream(throwingStream));

    std::optional<axtp::RpcPayload> endpointCallbackRequest;
    axtp::sdk::CallOptions endpointCallbackOptions;
    endpointCallbackOptions.timeout = std::chrono::milliseconds{100};
    const auto endpointCallbackFailure = endpointCallbackClient.callRaw(
        makeRequest(0x90010020), endpointCallbackOptions);
    REQUIRE(endpointCallbackFailure.statusCode == axtp::ErrorCode::InternalError);
    endpointCallbackRequest = takeOutgoingRequest(*endpointCallbackTransportPtr);
    REQUIRE(endpointCallbackRequest.has_value());
    endpointCallbackTransportPtr->injectIncoming(encodeRpc(makeResponse(*endpointCallbackRequest)));

    std::size_t endpointCallbackProbePolls = 0;
    axtp::sdk::CallOptions endpointCallbackProbe;
    endpointCallbackProbe.timeout = std::chrono::milliseconds{100};
    endpointCallbackProbe.acceptAnyResponse = true;
    endpointCallbackProbe.progress = [&] {
        (void)takeOutgoingRequest(*endpointCallbackTransportPtr);
        ++endpointCallbackProbePolls;
    };
    endpointCallbackProbe.cancelled = [&] { return endpointCallbackProbePolls == 1; };
    const auto endpointCallbackLateReply = endpointCallbackClient.callRaw(
        makeRequest(0x90010021), endpointCallbackProbe);
    REQUIRE(endpointCallbackLateReply.statusCode == axtp::ErrorCode::Canceled);

    // A transport poll is another endpoint callback boundary.  It must have
    // the same result/retirement behavior as a throwing broker callback.
    axtp::sdk::ClientOptions throwingPollClientOptions;
    throwingPollClientOptions.autoIdentify = false;
    axtp::sdk::AxtpClient throwingPollClient(throwingPollClientOptions);
    auto throwingPollTransport = std::make_unique<ThrowingPollTransport>();
    throwingPollClient.attachTransport(std::move(throwingPollTransport));
    const auto throwingPollFailure =
        throwingPollClient.callRaw(makeRequest(0x90010022));
    REQUIRE(throwingPollFailure.statusCode == axtp::ErrorCode::InternalError);

    // Standalone setup has the same poll/progress/cancel boundary as setup
    // entered by callRaw.  Cancellation is checked after progress, under the
    // one deadline established by ensureAppReady().
    axtp::sdk::AxtpClient standaloneAppReadyClient;
    standaloneAppReadyClient.attachTransport(std::make_unique<axtp::MockTransport>());
    std::size_t standaloneProgressCalls = 0;
    axtp::sdk::AppReadyOptions standaloneAppReadyOptions;
    standaloneAppReadyOptions.timeout = std::chrono::milliseconds{100};
    standaloneAppReadyOptions.progress = [&] { ++standaloneProgressCalls; };
    standaloneAppReadyOptions.cancelled = [&] { return standaloneProgressCalls == 1; };
    const auto standaloneCancelled =
        standaloneAppReadyClient.ensureAppReady(standaloneAppReadyOptions);
    REQUIRE(!standaloneCancelled.ok);
    REQUIRE(standaloneCancelled.statusCode == axtp::ErrorCode::Canceled);
    REQUIRE(standaloneCancelled.stage == "control-accept");
    REQUIRE(standaloneProgressCalls == 1);

    axtp::sdk::AxtpClient standaloneAppReadyExceptionClient;
    standaloneAppReadyExceptionClient.attachTransport(std::make_unique<axtp::MockTransport>());
    axtp::sdk::AppReadyOptions standaloneAppReadyExceptionOptions;
    standaloneAppReadyExceptionOptions.progress = [] {
        throw std::runtime_error("app-ready progress failed");
    };
    const auto standaloneProgressFailure =
        standaloneAppReadyExceptionClient.ensureAppReady(standaloneAppReadyExceptionOptions);
    REQUIRE(!standaloneProgressFailure.ok);
    REQUIRE(standaloneProgressFailure.statusCode == axtp::ErrorCode::InternalError);

    // A synchronous RPC wait must still provide a poll/progress opportunity
    // for media.  This reproduces the two-second control delay that used to
    // starve the stream callback: 30 fps is injected while the response is
    // withheld, then the response completes the same call.
    axtp::sdk::ClientOptions delayedRpcClientOptions;
    delayedRpcClientOptions.autoIdentify = false;
    axtp::sdk::AxtpClient delayedRpcClient(delayedRpcClientOptions);
    auto delayedRpcTransport = std::make_unique<axtp::MockTransport>();
    auto* delayedRpcTransportPtr = delayedRpcTransport.get();
    delayedRpcClient.attachTransport(std::move(delayedRpcTransport));
    std::size_t delayedRpcStreams = 0;
    delayedRpcClient.setStreamHandler(
        [&delayedRpcStreams](const axtp::BrokerContext&, const axtp::StreamPayload&) {
            ++delayedRpcStreams;
        });
    std::optional<axtp::RpcPayload> delayedRpcRequest;
    std::size_t delayedRpcProgressCalls = 0;
    std::uint32_t delayedRpcSeq = 1;
    bool delayedRpcResponded = false;
    const auto delayedRpcStarted = std::chrono::steady_clock::now();
    auto delayedRpcLastFrame = delayedRpcStarted;
    axtp::sdk::CallOptions delayedRpcOptions;
    delayedRpcOptions.timeout = std::chrono::milliseconds{2500};
    delayedRpcOptions.progress = [&] {
        ++delayedRpcProgressCalls;
        if (!delayedRpcRequest.has_value()) {
            delayedRpcRequest = takeOutgoingRequest(*delayedRpcTransportPtr);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - delayedRpcLastFrame >= std::chrono::milliseconds{33}) {
            axtp::StreamPayload media;
            media.streamId = 0x2001;
            media.seqId = delayedRpcSeq++;
            media.cursor = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - delayedRpcStarted)
                    .count());
            media.data = {0x00, 0x01, 0x02};
            delayedRpcTransportPtr->injectIncoming(encodeStream(std::move(media)));
            delayedRpcLastFrame = now;
        }
        if (!delayedRpcResponded && now - delayedRpcStarted >= std::chrono::seconds{2}) {
            delayedRpcTransportPtr->injectIncoming(
                encodeRpc(makeResponse(*delayedRpcRequest)));
            delayedRpcResponded = true;
        }
    };
    const auto delayedRpcResponse =
        delayedRpcClient.callRaw(makeRequest(0x90010016), delayedRpcOptions);
    const auto delayedRpcElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - delayedRpcStarted);
    REQUIRE(delayedRpcResponse.statusCode == axtp::ErrorCode::Success);
    REQUIRE(delayedRpcRequest.has_value());
    REQUIRE(delayedRpcResponded);
    REQUIRE(delayedRpcProgressCalls >= 100);
    REQUIRE(delayedRpcStreams >= 50);
    REQUIRE(delayedRpcElapsed >= std::chrono::milliseconds{1900});
    REQUIRE(delayedRpcElapsed < std::chrono::milliseconds{2400});

    // The automatic allocator must skip zero and every id already retired in
    // the current physical session when the 32-bit counter wraps.  Delayed
    // responses for either retired id must not escape through the legacy
    // acceptAnyResponse path and complete the wrapped request.
    axtp::sdk::ClientOptions wrapClientOptions;
    wrapClientOptions.autoIdentify = false;
    axtp::sdk::AxtpClient wrapClient(wrapClientOptions);
    auto wrapTransport = std::make_unique<axtp::MockTransport>();
    auto* wrapTransportPtr = wrapTransport.get();
    wrapClient.attachTransport(std::move(wrapTransport));

    auto retireOneRequest = makeRequest(0x90010017);
    retireOneRequest.requestId = 1;
    std::optional<axtp::RpcPayload> retiredOne;
    axtp::sdk::CallOptions retireOneOptions;
    retireOneOptions.timeout = std::chrono::milliseconds{100};
    retireOneOptions.progress = [&] {
        retiredOne = takeOutgoingRequest(*wrapTransportPtr);
        REQUIRE(retiredOne->requestId == 1);
        wrapTransportPtr->injectIncoming(encodeRpc(makeResponse(*retiredOne)));
    };
    const auto retireOneResponse =
        wrapClient.callRaw(std::move(retireOneRequest), retireOneOptions);
    REQUIRE(retireOneResponse.statusCode == axtp::ErrorCode::Success);
    REQUIRE(retireOneResponse.requestId == 1);
    REQUIRE(retiredOne.has_value());

    wrapClient.setNextRequestIdForTesting(0xFFFF'FFFFU);
    std::optional<axtp::RpcPayload> maximumIdRequest;
    axtp::sdk::CallOptions maximumIdOptions;
    maximumIdOptions.timeout = std::chrono::milliseconds{100};
    maximumIdOptions.progress = [&] {
        maximumIdRequest = takeOutgoingRequest(*wrapTransportPtr);
        wrapTransportPtr->injectIncoming(
            encodeRpc(makeResponse(*maximumIdRequest)));
    };
    const auto maximumIdResponse =
        wrapClient.callRaw(makeRequest(0x90010018), maximumIdOptions);
    REQUIRE(maximumIdResponse.statusCode == axtp::ErrorCode::Success);
    REQUIRE(maximumIdRequest.has_value());
    REQUIRE(maximumIdRequest->requestId == 0xFFFF'FFFFU);
    REQUIRE(maximumIdResponse.requestId == maximumIdRequest->requestId);

    std::optional<axtp::RpcPayload> wrappedRequest;
    std::size_t wrappedProgressCalls = 0;
    axtp::sdk::CallOptions wrappedOptions;
    wrappedOptions.timeout = std::chrono::milliseconds{100};
    wrappedOptions.acceptAnyResponse = true;
    wrappedOptions.progress = [&] {
        if (!wrappedRequest.has_value()) {
            wrappedRequest = takeOutgoingRequest(*wrapTransportPtr);
        }
        if (wrappedProgressCalls++ == 0) {
            wrapTransportPtr->injectIncoming(encodeRpc(makeResponse(*retiredOne)));
            wrapTransportPtr->injectIncoming(
                encodeRpc(makeResponse(*maximumIdRequest)));
            return;
        }
        wrapTransportPtr->injectIncoming(encodeRpc(makeResponse(*wrappedRequest)));
    };
    const auto wrappedResponse =
        wrapClient.callRaw(makeRequest(0x90010019), wrappedOptions);
    REQUIRE(wrappedResponse.statusCode == axtp::ErrorCode::Success);
    REQUIRE(wrappedRequest.has_value());
    REQUIRE(wrappedRequest->requestId == 2);
    REQUIRE(wrappedResponse.requestId == wrappedRequest->requestId);
    REQUIRE(wrappedProgressCalls == 2);

    // Explicit ids are single-use for a physical session.  This prevents a
    // delayed response from being paired with a later application request.
    auto explicitRequest = makeRequest(0x90010009);
    explicitRequest.requestId = 0xFFFF'FFFEU;
    const auto explicitResponse = client.callRaw(explicitRequest, finalOptions);
    REQUIRE(explicitResponse.statusCode == axtp::ErrorCode::Success);
    const auto reusedResponse = client.callRaw(explicitRequest, finalOptions);
    REQUIRE(reusedResponse.statusCode == axtp::ErrorCode::InvalidArgument);

    // A duplicate late response and an unrelated non-zero response must both
    // be consumed rather than escaping through acceptAnyResponse.
    transportPtr->injectIncoming(encodeRpc(makeResponse(*cancelledRequest)));
    std::size_t unknownResponsePolls = 0;
    axtp::sdk::CallOptions unknownResponseOptions;
    unknownResponseOptions.timeout = std::chrono::milliseconds{100};
    unknownResponseOptions.acceptAnyResponse = true;
    unknownResponseOptions.progress = [&] {
        if (unknownResponsePolls++ == 0) {
            const auto request = takeOutgoingRequest(*transportPtr);
            auto unknown = makeResponse(request);
            unknown.requestId = 0x7FFF'FFFEU;
            unknown.meta.requestId = unknown.requestId;
            transportPtr->injectIncoming(encodeRpc(std::move(unknown)));
        }
    };
    unknownResponseOptions.cancelled = [&] { return unknownResponsePolls >= 2; };
    const auto unknownResponse = client.callRaw(makeRequest(0x9001000A), unknownResponseOptions);
    REQUIRE(unknownResponse.statusCode == axtp::ErrorCode::Canceled);

    std::optional<axtp::RpcPayload> fallbackRequest;
    axtp::sdk::CallOptions fallbackOptions;
    fallbackOptions.timeout = std::chrono::milliseconds{100};
    fallbackOptions.acceptAnyResponse = true;
    fallbackOptions.progress = [&] {
        fallbackRequest = takeOutgoingRequest(*transportPtr);
        auto fallbackResponse = makeResponse(*fallbackRequest);
        fallbackResponse.requestId = 0;
        fallbackResponse.meta.requestId = 0;
        transportPtr->injectIncoming(encodeRpc(std::move(fallbackResponse)));
    };
    const auto fallbackResponse =
        client.callRaw(makeRequest(0x90010007), fallbackOptions);
    REQUIRE(fallbackResponse.statusCode == axtp::ErrorCode::Success);
    REQUIRE(fallbackResponse.requestId == 0);
    REQUIRE(fallbackRequest.has_value());

    transportPtr->injectIncoming(encodeRpc(makeResponse(*fallbackRequest)));
    const auto fallbackProbe =
        client.callRaw(makeRequest(0x90010008), cancelProbeOptions);
    REQUIRE(fallbackProbe.statusCode == axtp::ErrorCode::Canceled);
    REQUIRE(fallbackProbe.requestId != fallbackRequest->requestId);

    axtp::AxtpCore jsonCore;
    axtp::TransportProfile jsonProfile;
    jsonProfile.kind = axtp::TransportKind::WebSocket;
    jsonProfile.wireMode = axtp::AxtpWireMode::WebSocketJsonRpc;
    jsonProfile.defaultRpcEncoding = axtp::RpcEncoding::Json;
    jsonProfile.messageOriented = true;
    jsonProfile.supportsTextMessage = true;
    jsonProfile.supportsBinaryMessage = false;
    jsonCore.configure(jsonProfile);
    jsonCore.expectRpcResponse(77);
    jsonCore.abandonRpcResponse(77);

    const std::string lateJsonResponse =
        R"({"sid":"12345678","op":8,"d":{"id":77,"status":{"ok":true,"code":0},"result":{}}})";
    jsonCore.byteSink().onBytes(
        reinterpret_cast<const axtp::Byte*>(lateJsonResponse.data()),
        lateJsonResponse.size());
    REQUIRE(!jsonCore.tryTakeAnyRpcResponse().has_value());
    REQUIRE(!jsonCore.tryPopOutboundBytes().has_value());

    axtp::PendingCallTable retirementTable;
    for (std::uint32_t requestId = 1; requestId <= 1025; ++requestId) {
        retirementTable.expect(requestId);
        retirementTable.abandon(requestId);
    }
    REQUIRE(retirementTable.isRetired(1));
    REQUIRE(retirementTable.isRetired(2));
    REQUIRE(retirementTable.isRetired(2));

    retirementTable.abandon(2000);
    REQUIRE(!retirementTable.expect(2000));
    REQUIRE(retirementTable.isRetired(2000));

    retirementTable.abandon(2001);
    REQUIRE(retirementTable.isRetired(2001));
    REQUIRE(retirementTable.isRetired(2001));

    // Retirement is enforced below the SDK as well.  A direct Endpoint user
    // receives an explicit failure and the reused request is not written to
    // the transport after either success or abandonment.
    axtp::BasicBroker<> directBroker;
    axtp::AxtpEndpoint<axtp::BasicBroker<>> directEndpoint(directBroker);
    axtp::MockTransport directTransport;
    directEndpoint.attachTransport(directTransport);

    auto directRequest = makeRequest(0x9001000F);
    directRequest.requestId = 3000;
    directRequest.meta.requestId = directRequest.requestId;
    REQUIRE(directEndpoint.sendRpcRequest(directRequest));
    REQUIRE(directTransport.tryPopOutgoing().has_value());
    directTransport.injectIncoming(encodeRpc(makeResponse(directRequest)));
    const auto directResponse =
        directEndpoint.tryTakeRpcResponse(directRequest.requestId);
    REQUIRE(directResponse.has_value());
    REQUIRE(directResponse->statusCode == axtp::ErrorCode::Success);
    REQUIRE(!directEndpoint.sendRpcRequest(directRequest));
    REQUIRE(!directTransport.tryPopOutgoing().has_value());

    auto abandonedDirectRequest = makeRequest(0x90010010);
    abandonedDirectRequest.requestId = 3001;
    abandonedDirectRequest.meta.requestId = abandonedDirectRequest.requestId;
    REQUIRE(directEndpoint.sendRpcRequest(abandonedDirectRequest));
    REQUIRE(directTransport.tryPopOutgoing().has_value());
    directEndpoint.abandonRpcResponse(abandonedDirectRequest.requestId);
    REQUIRE(!directEndpoint.sendRpcRequest(abandonedDirectRequest));
    REQUIRE(!directTransport.tryPopOutgoing().has_value());

    // Auto-identify is part of the call's deadline/cancellation domain, not
    // an extra 5-second wait before the actual control operation begins.
    axtp::sdk::AxtpClient identifyingClient;
    identifyingClient.attachTransport(std::make_unique<JsonMockTransport>());
    std::size_t identifyProgress = 0;
    axtp::sdk::CallOptions identifyCancel;
    identifyCancel.timeout = std::chrono::milliseconds{0};
    identifyCancel.progress = [&] { ++identifyProgress; };
    identifyCancel.cancelled = [] { return true; };
    const auto identifyCancelled =
        identifyingClient.callRaw(makeRequest(0x9001000B), identifyCancel);
    REQUIRE(identifyCancelled.statusCode == axtp::ErrorCode::Canceled);
    REQUIRE(identifyProgress == 1);

    // A late ACCEPT from a cancelled attempt must remain queued while the
    // next attempt selects only the ACCEPT carrying its own control id.
    axtp::sdk::AxtpClient controlRetryClient;
    auto controlRetryTransport = std::make_unique<axtp::MockTransport>();
    auto* controlRetryTransportPtr = controlRetryTransport.get();
    controlRetryClient.attachTransport(std::move(controlRetryTransport));

    std::optional<axtp::ControlPayload> cancelledOpen;
    axtp::sdk::CallOptions cancelControlOpen;
    cancelControlOpen.timeout = std::chrono::milliseconds{100};
    cancelControlOpen.progress = [&] {
        auto outgoing = takeOutgoingPayloads(*controlRetryTransportPtr);
        REQUIRE(outgoing.controls.size() == 1);
        REQUIRE(outgoing.controls.front().opcode == axtp::ControlOpcode::Open);
        cancelledOpen = std::move(outgoing.controls.front());
    };
    cancelControlOpen.cancelled = [&] { return cancelledOpen.has_value(); };
    const auto controlOpenCancelled =
        controlRetryClient.callRaw(makeRequest(0x90010011), cancelControlOpen);
    REQUIRE(controlOpenCancelled.statusCode == axtp::ErrorCode::Canceled);
    REQUIRE(controlRetryClient.lastAppReadyResult().stage == "control-accept");
    REQUIRE(cancelledOpen.has_value());

    controlRetryTransportPtr->injectIncoming(
        encodeControl(makeAccept(*cancelledOpen)));

    std::size_t retryOpenCount = 0;
    std::size_t retryIdentifyCount = 0;
    std::size_t retryRequestCount = 0;
    axtp::sdk::CallOptions completeRetry;
    completeRetry.timeout = std::chrono::milliseconds{100};
    completeRetry.progress = [&] {
        while (auto outgoing =
                   tryTakeOutgoingPayloads(*controlRetryTransportPtr)) {
            for (const auto& control : outgoing->controls) {
                REQUIRE(control.opcode == axtp::ControlOpcode::Open);
                ++retryOpenCount;
                controlRetryTransportPtr->injectIncoming(
                    encodeControl(makeAccept(control)));
                controlRetryTransportPtr->injectIncoming(
                    encodeRpc(axtp::JsonRpcEncoder::makeHello()));
            }
            for (const auto& rpc : outgoing->rpcs) {
                if (rpc.op == axtp::RpcOp::Identify) {
                    ++retryIdentifyCount;
                    controlRetryTransportPtr->injectIncoming(
                        encodeRpc(axtp::JsonRpcEncoder::makeIdentified("fresh-sid")));
                } else if (rpc.op == axtp::RpcOp::Request) {
                    ++retryRequestCount;
                    controlRetryTransportPtr->injectIncoming(
                        encodeRpc(makeResponse(rpc)));
                } else {
                    REQUIRE(false);
                }
            }
        }
    };
    const auto retriedAfterLateAccept =
        controlRetryClient.callRaw(makeRequest(0x90010012), completeRetry);
    REQUIRE(retriedAfterLateAccept.statusCode == axtp::ErrorCode::Success);
    REQUIRE(retryOpenCount == 1);
    REQUIRE(retryIdentifyCount == 1);
    REQUIRE(retryRequestCount == 1);

    // Auto-identification and the business response share one absolute
    // timeout budget.  Completing the handshake near the deadline must not
    // grant the request a second full timeout window.
    axtp::sdk::AxtpClient deadlineClient;
    auto deadlineTransport = std::make_unique<axtp::MockTransport>();
    auto* deadlineTransportPtr = deadlineTransport.get();
    deadlineClient.attachTransport(std::move(deadlineTransport));
    bool delayedControlOpen = false;
    bool deadlineRequestSent = false;
    axtp::sdk::CallOptions absoluteDeadline;
    absoluteDeadline.timeout = std::chrono::milliseconds{200};
    absoluteDeadline.progress = [&] {
        while (auto outgoing =
                   tryTakeOutgoingPayloads(*deadlineTransportPtr)) {
            for (const auto& control : outgoing->controls) {
                REQUIRE(control.opcode == axtp::ControlOpcode::Open);
                if (!delayedControlOpen) {
                    delayedControlOpen = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds{140});
                }
                deadlineTransportPtr->injectIncoming(
                    encodeControl(makeAccept(control)));
                deadlineTransportPtr->injectIncoming(
                    encodeRpc(axtp::JsonRpcEncoder::makeHello()));
            }
            for (const auto& rpc : outgoing->rpcs) {
                if (rpc.op == axtp::RpcOp::Identify) {
                    deadlineTransportPtr->injectIncoming(
                        encodeRpc(axtp::JsonRpcEncoder::makeIdentified("deadline-sid")));
                } else if (rpc.op == axtp::RpcOp::Request) {
                    deadlineRequestSent = true;
                } else {
                    REQUIRE(false);
                }
            }
        }
    };
    const auto deadlineStarted = std::chrono::steady_clock::now();
    const auto absoluteDeadlineResult =
        deadlineClient.callRaw(makeRequest(0x90010015), absoluteDeadline);
    const auto deadlineElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - deadlineStarted);
    REQUIRE(absoluteDeadlineResult.statusCode ==
            axtp::ErrorCode::RpcResponseTimeout);
    REQUIRE(delayedControlOpen);
    REQUIRE(deadlineRequestSent);
    REQUIRE(deadlineElapsed >= std::chrono::milliseconds{140});
    REQUIRE(deadlineElapsed < std::chrono::milliseconds{300});

    // IDENTIFIED has no request id.  Cancellation after IDENTIFY therefore
    // resumes the original wait on the same physical session; it must not
    // send another OPEN/IDENTIFY and misassociate the late response.
    axtp::sdk::AxtpClient identifyResumeClient;
    auto identifyResumeTransport = std::make_unique<axtp::MockTransport>();
    auto* identifyResumeTransportPtr = identifyResumeTransport.get();
    identifyResumeClient.attachTransport(std::move(identifyResumeTransport));

    std::size_t resumeOpenCount = 0;
    std::size_t resumeIdentifyCount = 0;
    std::size_t resumeRequestCount = 0;
    bool cancelIdentifiedWait = false;
    auto serviceIdentifyResumeTransport = [&] {
        while (auto outgoing =
                   tryTakeOutgoingPayloads(*identifyResumeTransportPtr)) {
            for (const auto& control : outgoing->controls) {
                REQUIRE(control.opcode == axtp::ControlOpcode::Open);
                ++resumeOpenCount;
                identifyResumeTransportPtr->injectIncoming(
                    encodeControl(makeAccept(control)));
                identifyResumeTransportPtr->injectIncoming(
                    encodeRpc(axtp::JsonRpcEncoder::makeHello()));
            }
            for (const auto& rpc : outgoing->rpcs) {
                if (rpc.op == axtp::RpcOp::Identify) {
                    ++resumeIdentifyCount;
                    cancelIdentifiedWait = true;
                } else if (rpc.op == axtp::RpcOp::Request) {
                    ++resumeRequestCount;
                    identifyResumeTransportPtr->injectIncoming(
                        encodeRpc(makeResponse(rpc)));
                } else {
                    REQUIRE(false);
                }
            }
        }
    };

    axtp::sdk::CallOptions cancelAfterIdentify;
    cancelAfterIdentify.timeout = std::chrono::milliseconds{100};
    cancelAfterIdentify.progress = serviceIdentifyResumeTransport;
    cancelAfterIdentify.cancelled = [&] { return cancelIdentifiedWait; };
    const auto identifiedWaitCancelled =
        identifyResumeClient.callRaw(makeRequest(0x90010013), cancelAfterIdentify);
    REQUIRE(identifiedWaitCancelled.statusCode == axtp::ErrorCode::Canceled);
    REQUIRE(identifyResumeClient.lastAppReadyResult().stage == "identified");
    REQUIRE(resumeOpenCount == 1);
    REQUIRE(resumeIdentifyCount == 1);
    REQUIRE(resumeRequestCount == 0);

    identifyResumeTransportPtr->injectIncoming(
        encodeRpc(axtp::JsonRpcEncoder::makeIdentified("resumed-sid")));
    axtp::sdk::CallOptions resumeAfterIdentified;
    resumeAfterIdentified.timeout = std::chrono::milliseconds{100};
    resumeAfterIdentified.progress = serviceIdentifyResumeTransport;
    const auto resumedCall =
        identifyResumeClient.callRaw(makeRequest(0x90010014), resumeAfterIdentified);
    REQUIRE(resumedCall.statusCode == axtp::ErrorCode::Success);
    REQUIRE(identifyResumeClient.sessionSid() == "resumed-sid");
    REQUIRE(resumeOpenCount == 1);
    REQUIRE(resumeIdentifyCount == 1);
    REQUIRE(resumeRequestCount == 1);
}
