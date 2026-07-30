#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#    include <process.h>
#else
#    include <unistd.h>
#endif

#include "axtp_core.hpp"
#include "core/protocol/generated/method_traits.h"
#include "core/protocol/generated/schema_codec.h"

#include "sdk/app_ready_options.hpp"
#include "sdk/call_options.hpp"
#include "sdk/client_options.hpp"
#include "sdk/endpoints.hpp"
#include "sdk/sdk_error.hpp"

namespace axtp::sdk {

class AxtpClient {
public:
    using RawMethodHandler = std::function<Bytes(const RpcPayload&)>;
    using RawEventHandler = std::function<void(const RpcPayload&)>;
    using IngressTokenProvider = AxtpCore::IngressTokenProvider;

    explicit AxtpClient(ClientOptions options = {})
        : _options(options)
        , _endpoint(std::make_unique<AxtpEndpoint<BasicBroker<>>>(_broker)) {
        _broker.registerEventHandler(
            [this](const BrokerContext&, const RpcPayload& eventPayload) {
                emitRaw(eventPayload);
            });
    }

    ~AxtpClient() {
        close();
    }

    void attachTransport(std::unique_ptr<ITransport> transport) {
        close();
        _transport = std::move(transport);
        _endpoint = std::make_unique<AxtpEndpoint<BasicBroker<>>>(_broker);
        _appReady = false;
        _sessionSid.clear();
        _identifyOutstanding = false;
        _pendingIdentifyRandomSeed.reset();
        _negotiatedHeartbeatIntervalMs.reset();
        _usedRequestIds.clear();
        if (_transport != nullptr) {
            _endpoint->attachTransport(*_transport);
            // attachTransport() creates a fresh endpoint/core.  Reapply the
            // provider after that replacement so deferred stream tasks retain
            // the raw-ingress lease token on every physical session.
            _endpoint->setIngressTokenProvider(_ingressTokenProvider);
            if (_options.autoOpen) {
                _transport->open();
            }
            _connected = true;
        }
    }

    void connect(const TcpEndpoint& endpoint) {
        (void)endpoint;
        _lastError =
            SdkError::failure(ErrorCode::NotSupported,
                              "TCP transport construction is provided by optional connectors");
        _connected = false;
    }

    void connect(const WebSocketEndpoint& endpoint) {
        _options.wireMode = endpoint.wireMode;
        _lastError = SdkError::failure(
            ErrorCode::NotSupported,
            "WebSocket transport construction is provided by optional connectors");
        _connected = false;
    }

    void connect(const HidEndpoint& endpoint) {
        (void)endpoint;
        _lastError =
            SdkError::failure(ErrorCode::NotSupported,
                              "HID transport construction is provided by optional connectors");
        _connected = false;
    }

    void connect(const BleEndpoint&) {
        _lastError = SdkError::failure(ErrorCode::NotSupported,
                                       "BLE client transport is not implemented in P0");
    }

    void connect(const UartEndpoint&) {
        _lastError = SdkError::failure(ErrorCode::NotSupported,
                                       "UART client transport is not implemented in P0");
    }

    void close() {
        if (_transport != nullptr) {
            _transport->close();
        }
        _connected = false;
        _appReady = false;
        _sessionSid.clear();
        _identifyOutstanding = false;
        _pendingIdentifyRandomSeed.reset();
        _negotiatedHeartbeatIntervalMs.reset();
        _usedRequestIds.clear();
        if (_endpoint != nullptr) {
            _endpoint->core().resetResponseTracking();
        }
    }

    bool isConnected() const {
        return _connected;
    }

    const SdkError& lastError() const {
        return _lastError;
    }

    std::optional<std::uint32_t> negotiatedHeartbeatIntervalMs() const {
        return _negotiatedHeartbeatIntervalMs;
    }

    std::uint64_t inboundActivityGeneration() const {
        return _endpoint == nullptr ? 0 : _endpoint->inboundActivityGeneration();
    }

    // Set before or after attachTransport().  The value is retained across
    // endpoint replacement and is consulted only at decoded stream ingress.
    void setIngressTokenProvider(IngressTokenProvider provider) {
        _ingressTokenProvider = std::move(provider);
        if (_endpoint != nullptr) {
            _endpoint->setIngressTokenProvider(_ingressTokenProvider);
        }
    }

#if defined(AXTP_RUNTIME_TESTING)
    // Test-only seam for exercising the 32-bit allocator wrap without
    // issuing billions of requests. It is not present in production builds.
    void setNextRequestIdForTesting(std::uint32_t requestId) noexcept {
        _nextRequestId = requestId;
    }
#endif

    // Send one control HEARTBEAT and wait for the matching ACK.  This is a
    // protocol-level operation and is deliberately separate from RPC calls so
    // a peer can answer it without waking the business broker.  The optional
    // progress hook runs after every poll, before ACK matching, so an owner
    // that is also carrying media can drain its staging queue while a quiet
    // peer takes the full heartbeat timeout.  The one-argument API remains
    // source-compatible for callers that do not need that hook.
    SdkError heartbeat(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{500},
        std::function<void()> progress = {}) {
        if (_transport == nullptr || _endpoint == nullptr || !_connected) {
            _lastError = SdkError::failure(ErrorCode::Unavailable, "transport unavailable");
            return _lastError;
        }
        if (_transport->profile().wireMode != AxtpWireMode::FramedBinary ||
            !_endpoint->core().controlSessionOpen()) {
            _lastError = SdkError::failure(
                ErrorCode::NotSupported,
                "control heartbeat requires an open framed-binary session");
            return _lastError;
        }
        if (!_negotiatedHeartbeatIntervalMs.has_value()) {
            _lastError = SdkError::failure(
                ErrorCode::NotSupported,
                "peer did not advertise a valid heartbeat interval");
            return _lastError;
        }
        if (timeout.count() <= 0) {
            timeout = std::chrono::milliseconds{1};
        }
        auto controlId = _nextControlId++;
        if (controlId == 0) {
            controlId = _nextControlId++;
        }
        try {
            _endpoint->sendControlHeartbeat(controlId);
        } catch (...) {
            _lastError = SdkError::failure(
                ErrorCode::InternalError,
                "heartbeat endpoint callback failed");
            return _lastError;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                poll();
            } catch (...) {
                _lastError = SdkError::failure(
                    ErrorCode::InternalError,
                    "heartbeat endpoint callback failed");
                return _lastError;
            }
            if (progress) {
                try {
                    progress();
                } catch (...) {
                    _lastError = SdkError::failure(
                        ErrorCode::InternalError,
                        "heartbeat progress callback failed");
                    return _lastError;
                }
            }
            if (auto ack = _endpoint->tryTakeControlNotice(
                    ControlOpcode::HeartbeatAck, controlId)) {
                if (ack->statusCode == ErrorCode::Success) {
                    _lastError = SdkError::success();
                    return _lastError;
                }
                _lastError = SdkError::failure(
                    ack->statusCode, "control heartbeat rejected by peer");
                return _lastError;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        _lastError = SdkError::failure(
            ErrorCode::ControlHeartbeatTimeout,
            "control heartbeat acknowledgement timed out");
        return _lastError;
    }

    MethodRegistry& registry() {
        return _registry;
    }

    const MethodRegistry& registry() const {
        return _registry;
    }

    void poll() {
        if (_endpoint != nullptr) {
            _endpoint->poll();
        }
    }

    AppReadyResult ensureAppReady(AppReadyOptions options = {}) {
        const auto deadline = deadlineAfter(options.timeout);
        return ensureAppReadyUntil(std::move(options), deadline, nullptr);
    }

private:
    // callRaw uses this form so automatic identification consumes the same
    // absolute timeout budget as the business request.  The public overload
    // above retains the SDK's existing API for callers that only need setup.
    AppReadyResult ensureAppReadyUntil(AppReadyOptions options,
                                       std::chrono::steady_clock::time_point deadline,
                                       const CallOptions* waitOptions) {
        try {
            return ensureAppReadyUntilImpl(std::move(options), deadline, waitOptions);
        } catch (...) {
            AppReadyResult result;
            result.statusCode = ErrorCode::InternalError;
            result.stage = "callback";
            _lastAppReady = result;
            _lastError = SdkError::failure(result.statusCode, "app-ready callback failed");
            return result;
        }
    }

    AppReadyResult ensureAppReadyUntilImpl(AppReadyOptions options,
                                           std::chrono::steady_clock::time_point deadline,
                                           const CallOptions* waitOptions) {
        AppReadyResult result;
        auto emitTrace = [&](std::string stage,
                             std::string action,
                             ErrorCode statusCode = ErrorCode::Success,
                             std::uint16_t controlId = 0,
                             std::string sid = {},
                             std::string bodyText = {},
                             std::string detail = {},
                             bool includeRandomSeed = false) {
            if (!options.trace) {
                return;
            }
            AppReadyTraceEvent event;
            event.stage = std::move(stage);
            event.action = std::move(action);
            event.statusCode = statusCode;
            event.controlId = controlId;
            event.hasRandomSeed = includeRandomSeed && result.hasRandomSeed;
            event.randomSeed = event.hasRandomSeed ? result.randomSeed : 0;
            event.sid = std::move(sid);
            event.bodyText = std::move(bodyText);
            event.detail = std::move(detail);
            options.trace(event);
        };

        emitTrace("start", "begin", ErrorCode::Success, 0, "", "", "ensureAppReady entered");

        if (_appReady && !_sessionSid.empty()) {
            result.ok = true;
            result.stage = "app-ready";
            result.sid = _sessionSid;
            result.hasRandomSeed = _lastAppReady.hasRandomSeed;
            result.randomSeed = _lastAppReady.randomSeed;
            emitTrace("app-ready", "already-ready", ErrorCode::Success, 0, result.sid, "", "", true);
            _lastAppReady = result;
            _lastError = SdkError::success();
            return result;
        }

        if (_transport == nullptr || _endpoint == nullptr) {
            result.statusCode = ErrorCode::Unavailable;
            result.stage = "transport";
            emitTrace("transport", "error", result.statusCode, 0, "", "", "transport unavailable");
            _lastAppReady = result;
            _lastError = SdkError::failure(result.statusCode, "transport unavailable");
            return result;
        }

        const auto profile = _transport->profile();
        _endpoint->core().configure(profile);
        _endpoint->core().setRequestedHeartbeatInterval(
            static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                _options.requestedHeartbeatInterval.count(), 1, 60000)));
        auto stopWaiting = [&](std::string_view stage) -> std::optional<AppReadyResult> {
            if (const auto status = invokeProgress(options, waitOptions)) {
                result.statusCode = *status;
                result.stage = std::string(stage);
                emitTrace(result.stage, "error", result.statusCode, 0, "", "", "progress callback failed");
                _lastAppReady = result;
                _lastError = SdkError::failure(result.statusCode, "app-ready progress failed");
                return result;
            }
            return std::nullopt;
        };

        auto cancellationOrTimeout = [&](std::string_view stage) -> std::optional<AppReadyResult> {
            if (const auto status = invokeCancellation(options, waitOptions)) {
                result.statusCode = *status;
                result.stage = std::string(stage);
                emitTrace(result.stage, *status == ErrorCode::Canceled ? "cancelled" : "error", *status);
                _lastAppReady = result;
                _lastError = SdkError::failure(result.statusCode, "app-ready cancelled");
                return result;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result.statusCode = ErrorCode::RpcResponseTimeout;
                result.stage = std::string(stage);
                emitTrace(result.stage, "timeout", result.statusCode);
                _lastAppReady = result;
                _lastError = SdkError::failure(result.statusCode, "app-ready timeout");
                return result;
            }
            return std::nullopt;
        };

        auto waitForIdentified = [&]() -> AppReadyResult {
            result.stage = "identified";
            result.hasRandomSeed = _pendingIdentifyRandomSeed.has_value();
            result.randomSeed = _pendingIdentifyRandomSeed.value_or(0);
            emitTrace("identified", "wait", ErrorCode::Success, 0, "", "", "", true);
            while (true) {
                poll();
                if (const auto stopped = stopWaiting("identified")) {
                    return *stopped;
                }
                if (auto identified = _endpoint->tryTakeSessionRpc(RpcOp::Identified)) {
                    const std::string bodyText(identified->body.begin(), identified->body.end());
                    emitTrace("identified",
                              "receive",
                              ErrorCode::Success,
                              0,
                              identified->meta.jsonSid,
                              bodyText,
                              "",
                              true);
                    _identifyOutstanding = false;
                    _pendingIdentifyRandomSeed.reset();
                    if (!identified->meta.jsonSid.empty()) {
                        _sessionSid = identified->meta.jsonSid;
                        _appReady = true;
                        result.ok = true;
                        result.statusCode = ErrorCode::Success;
                        result.stage = "app-ready";
                        result.sid = _sessionSid;
                        emitTrace(
                            "app-ready", "ready", ErrorCode::Success, 0, result.sid, "", "", true);
                        _lastAppReady = result;
                        _lastError = SdkError::success();
                        return result;
                    }
                    result.statusCode = ErrorCode::RpcPayloadInvalid;
                    emitTrace("identified", "error", result.statusCode, 0, "", bodyText, "", true);
                    _lastAppReady = result;
                    _lastError = SdkError::failure(result.statusCode, "identified sid missing");
                    return result;
                }
                if (const auto stopped = cancellationOrTimeout("identified")) {
                    return *stopped;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };

        // IDENTIFIED has no request id on the wire.  If a wait was cancelled
        // or timed out after sending IDENTIFY, resume that exact wait instead
        // of opening a second session and allowing the late response to be
        // mistaken for a newer handshake.
        if (_identifyOutstanding) {
            emitTrace("identify",
                      "resume",
                      ErrorCode::Success,
                      0,
                      "",
                      "",
                      "waiting for the outstanding identify response",
                      true);
            return waitForIdentified();
        }

        if (profile.wireMode == AxtpWireMode::FramedBinary && !options.skipControlOpen) {
            result.stage = "control-open";
            const auto controlId = _nextControlId++;
            emitTrace("control-open", "send", ErrorCode::Success, controlId);
            _endpoint->sendControlOpen(controlId);
            emitTrace("control-accept", "wait", ErrorCode::Success, controlId);
            while (true) {
                poll();
                if (const auto stopped = stopWaiting("control-accept")) {
                    return *stopped;
                }
                // Match the acknowledgement to the Open we just sent before
                // consuming it.  A previous, cancelled handshake may still
                // deliver a late ACCEPT; consuming that notice as a generic
                // ACCEPT would incorrectly reject the new physical open.
                if (auto accept = _endpoint->tryTakeControlNotice(
                        ControlOpcode::Accept, controlId)) {
                    emitTrace("control-accept",
                              "receive",
                              accept->statusCode,
                              accept->controlId,
                              "",
                              "",
                              "controlId matched");
                    if (accept->statusCode == ErrorCode::Success &&
                        _endpoint->core().controlSessionOpen()) {
                        _negotiatedHeartbeatIntervalMs =
                            _endpoint->core().negotiatedHeartbeatIntervalMs();
                        emitTrace(
                            "framing-ready", "ready", ErrorCode::Success, accept->controlId);
                        break;
                    }
                    result.statusCode = accept->statusCode == ErrorCode::Success
                                            ? ErrorCode::ControlOpenRejected
                                            : accept->statusCode;
                    emitTrace("control-open", "error", result.statusCode, accept->controlId);
                    _lastAppReady = result;
                    _lastError = SdkError::failure(result.statusCode, "control open rejected");
                    return result;
                }
                if (const auto stopped = cancellationOrTimeout("control-accept")) {
                    return *stopped;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!_endpoint->core().controlSessionOpen()) {
                result.statusCode = ErrorCode::RpcResponseTimeout;
                emitTrace("control-accept", "timeout", result.statusCode, controlId);
                _lastAppReady = result;
                _lastError = SdkError::failure(result.statusCode, "control accept timeout");
                return result;
            }
        } else {
            emitTrace("control-open",
                      "skip",
                      ErrorCode::Success,
                      0,
                      "",
                      "",
                      profile.wireMode == AxtpWireMode::WebSocketJsonRpc ? "websocket-json-rpc"
                                                                          : "skipControlOpen");
        }

        result.stage = "hello";
        bool gotHello = false;
        emitTrace("hello", "wait");
        while (true) {
            poll();
            if (const auto stopped = stopWaiting("hello")) {
                return *stopped;
            }
            if (auto hello = _endpoint->tryTakeSessionRpc(RpcOp::Hello)) {
                gotHello = true;
                emitTrace("hello",
                          "receive",
                          ErrorCode::Success,
                          0,
                          hello->meta.jsonSid,
                          std::string(hello->body.begin(), hello->body.end()));
                break;
            }
            if (const auto stopped = cancellationOrTimeout("hello")) {
                return *stopped;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!gotHello) {
            result.statusCode = ErrorCode::RpcResponseTimeout;
            emitTrace("hello", "timeout", result.statusCode);
            _lastAppReady = result;
            _lastError = SdkError::failure(result.statusCode, "hello timeout");
            return result;
        }

        result.stage = "identify";
        result.hasRandomSeed = true;
        result.randomSeed = options.randomSeed.value_or(generateRandomSeed());
        emitTrace("identify",
                  "send",
                  ErrorCode::Success,
                  0,
                  "",
                  "",
                  std::string("randomSeed=") + std::to_string(result.randomSeed) +
                      " eventMasks=" + options.eventMasks,
                  true);
        _endpoint->sendRpcSession(
            JsonRpcEncoder::makeIdentify(result.randomSeed, options.eventMasks));
        _identifyOutstanding = true;
        _pendingIdentifyRandomSeed = result.randomSeed;
        return waitForIdentified();
    }

public:
    bool isAppReady() const {
        return _appReady;
    }

    const std::string& sessionSid() const {
        return _sessionSid;
    }

    const AppReadyResult& lastAppReadyResult() const {
        return _lastAppReady;
    }

    void registerMethod(std::uint32_t methodId, RawMethodHandler handler) {
        _localHandlers[methodId] = std::move(handler);
    }

    void registerEventHandler(std::uint32_t eventId, RawEventHandler handler) {
        _eventHandlers[eventId] = std::move(handler);
    }

    void setStreamHandler(BasicBroker<>::StreamHandler handler) {
        _broker.registerStreamHandler(std::move(handler));
    }

    RpcPayload callRaw(RpcPayload request, CallOptions options = {}) {
        const auto deadline = deadlineAfter(options.timeout);
        if (!normalizeRequest(request, options)) {
            return makeErrorResponse(request, ErrorCode::InvalidArgument);
        }

        // A request id only becomes unavailable for this physical session
        // once it can have reached the peer.  In particular, a caller may
        // retry an explicitly assigned id after local dispatch, an unavailable
        // transport, or an auto-identify failure: none of those paths writes
        // the business request, so there is no late response to quarantine.
        const auto releaseUnsentRequestId = [this, &request]() {
            _usedRequestIds.erase(request.requestId);
        };

        const auto local = _localHandlers.find(request.methodOrEventId);
        if (local != _localHandlers.end()) {
            RpcPayload response;
            response.encoding = request.encoding;
            response.op = RpcOp::RequestResponse;
            response.requestId = request.requestId;
            response.methodOrEventId = request.methodOrEventId;
            response.statusCode = ErrorCode::Success;
            response.bodyEncoding = request.bodyEncoding;
            response.meta = request.meta;
            try {
                response.body = local->second(request);
            } catch (...) {
                releaseUnsentRequestId();
                return makeErrorResponse(request, ErrorCode::InternalError);
            }
            releaseUnsentRequestId();
            return response;
        }

        if (_transport == nullptr || _endpoint == nullptr) {
            releaseUnsentRequestId();
            return makeErrorResponse(request, ErrorCode::Unavailable);
        }

        const auto profile = _transport->profile();
        _endpoint->core().configure(profile);
        if (_options.autoIdentify && !_appReady) {
            AppReadyOptions appOptions;
            appOptions.timeout = remainingUntil(deadline);
            const auto ready = ensureAppReadyUntil(appOptions, deadline, &options);
            if (!ready.ok) {
                releaseUnsentRequestId();
                return makeErrorResponse(request, ready.statusCode);
            }
            if (request.meta.sourceProtocol == SourceProtocol::JsonRpc &&
                request.meta.jsonSid.empty() && !_sessionSid.empty()) {
                request.meta.jsonSid = _sessionSid;
            }
        }
        try {
            if (!_endpoint->sendRpcRequest(request)) {
                // The Core rejects ids retired earlier in this physical session.
                // Do not enter the wait loop (and, importantly, do not put a
                // request on the wire) when a direct caller supplied a reused id.
                releaseUnsentRequestId();
                return makeErrorResponse(request, ErrorCode::InvalidArgument);
            }
        } catch (...) {
            // sendRpcRequest flushes through the endpoint.  A throwing
            // transport callback can therefore leave the request pending even
            // though the caller never receives a normal response.
            _endpoint->abandonRpcResponse(request.requestId);
            return makeErrorResponse(request, ErrorCode::InternalError);
        }

        while (true) {
            try {
                // Endpoint polling crosses transport and registered
                // event/stream handler boundaries.  Never allow either kind
                // of embedding callback to escape a synchronous RPC call.
                poll();
            } catch (...) {
                _endpoint->abandonRpcResponse(request.requestId);
                return makeErrorResponse(request, ErrorCode::InternalError);
            }
            if (const auto status = invokeProgress(options)) {
                _endpoint->abandonRpcResponse(request.requestId);
                return makeErrorResponse(request, *status);
            }
            if (auto response = _endpoint->tryTakeRpcResponse(request.requestId)) {
                return *response;
            }
            if (options.acceptAnyResponse) {
                if (auto response = _endpoint->tryTakeAnyRpcResponse()) {
                    // A legacy id-zero reply completes this call.  Retire the
                    // real request id so a later matching response cannot
                    // surface during a subsequent call.
                    _endpoint->abandonRpcResponse(request.requestId);
                    return *response;
                }
            }
            if (const auto status = invokeCancellation(options)) {
                _endpoint->abandonRpcResponse(request.requestId);
                return makeErrorResponse(request, *status);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                _endpoint->abandonRpcResponse(request.requestId);
                return makeErrorResponse(request, ErrorCode::RpcResponseTimeout);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    Bytes
    callRaw(std::uint32_t methodId, RpcEncoding encoding, Bytes body, CallOptions options = {}) {
        RpcPayload request = makeDynamicRequest(methodId, encoding, std::move(body), options);
        auto response = callRaw(std::move(request), options);
        _lastError = response.statusCode == ErrorCode::Success
                         ? SdkError::success()
                         : SdkError::failure(response.statusCode);
        return response.body;
    }

    std::string
    callJson(std::string_view methodName, std::string_view paramsJson, CallOptions options = {}) {
        const auto methodId = _registry.findMethodId(methodName);
        if (!methodId.has_value()) {
            _lastError = SdkError::failure(ErrorCode::RpcMethodNotFound, "method not found");
            return {};
        }
        options.encoding = RpcEncoding::Json;
        auto body = callJson(*methodId, paramsJson, options);
        return body;
    }

    std::string
    callJson(std::uint32_t methodId, std::string_view paramsJson, CallOptions options = {}) {
        options.encoding = RpcEncoding::Json;
        Bytes body(paramsJson.begin(), paramsJson.end());
        auto response = callRaw(methodId, RpcEncoding::Json, std::move(body), options);
        return std::string(response.begin(), response.end());
    }

    Bytes callTlv(std::string_view methodName, Bytes tlvBody, CallOptions options = {}) {
        const auto methodId = _registry.findMethodId(methodName);
        if (!methodId.has_value()) {
            _lastError = SdkError::failure(ErrorCode::RpcMethodNotFound, "method not found");
            return {};
        }
        return callTlv(*methodId, std::move(tlvBody), options);
    }

    Bytes callTlv(std::uint32_t methodId, Bytes tlvBody, CallOptions options = {}) {
        options.encoding = jsonBinaryRpcEncoding();
        return callRaw(methodId, jsonBinaryRpcEncoding(), std::move(tlvBody), options);
    }

    Bytes callRawBytes(std::uint32_t methodId, Bytes body, CallOptions options = {}) {
        options.encoding = jsonBinaryRpcEncoding();
        return callRaw(methodId, jsonBinaryRpcEncoding(), std::move(body), options);
    }

    template <MethodId Id>
    typename MethodTraits<Id>::Response callTyped(const typename MethodTraits<Id>::Request& request,
                                                  CallOptions options = {}) {
        auto payload = makeTypedRequest<Id>(request, options);
        auto response = callRaw(std::move(payload), options);
        return SchemaCodec::decodeResponse<Id>(response.body, response.encoding);
    }

    template <MethodId Id>
    typename MethodTraits<Id>::Response call(const typename MethodTraits<Id>::Request& request,
                                             CallOptions options = {}) {
        return callTyped<Id>(request, options);
    }

    template <MethodId Id>
    RpcPayload makeTypedRequest(const typename MethodTraits<Id>::Request& request,
                                CallOptions options = {}) {
        RpcPayload payload;
        payload.encoding = options.encoding;
        payload.op = RpcOp::Request;
        payload.methodOrEventId = MethodTraits<Id>::id;
        payload.bodyEncoding = SchemaCodec::bodyEncodingFor(options.encoding);
        payload.body = SchemaCodec::encodeRequest<Id>(request, options.encoding);
        payload.meta.sourceProtocol = options.encoding == RpcEncoding::Json
                                          ? SourceProtocol::JsonRpc
                                          : SourceProtocol::AxtpV1;
        payload.meta.jsonMethodOrEventName = MethodTraits<Id>::name;
        return payload;
    }

    void emitRaw(RpcPayload eventPayload) {
        const auto it = _eventHandlers.find(eventPayload.methodOrEventId);
        if (it != _eventHandlers.end()) {
            // Invoke a copy so a handler may safely replace its own registration.
            auto handler = it->second;
            if (handler) {
                handler(eventPayload);
            }
        }
    }

    void sendStream(StreamPayload payload) {
        if (_transport == nullptr || _endpoint == nullptr) {
            _lastError = SdkError::failure(ErrorCode::Unavailable, "transport unavailable");
            return;
        }
        _endpoint->core().configure(_transport->profile());
        _endpoint->sendStream(std::move(payload));
        _lastError = SdkError::success();
    }

private:
    static RpcBodyEncoding bodyEncodingFor(RpcEncoding encoding) {
        return bodyEncodingForRpcEncoding(encoding);
    }

    RpcPayload makeDynamicRequest(std::uint32_t methodId,
                                  RpcEncoding encoding,
                                  Bytes body,
                                  const CallOptions&) const {
        RpcPayload payload;
        payload.encoding = encoding;
        payload.op = RpcOp::Request;
        payload.methodOrEventId = methodId;
        payload.bodyEncoding = bodyEncodingFor(encoding);
        payload.body = std::move(body);
        payload.meta.sourceProtocol = encoding == RpcEncoding::Json ? SourceProtocol::JsonRpc
                                                                     : SourceProtocol::AxtpV1;
        if (const auto methodName = _registry.findMethodName(methodId)) {
            payload.meta.jsonMethodOrEventName = std::string(*methodName);
        }
        return payload;
    }

    static std::chrono::steady_clock::time_point deadlineAfter(
        std::chrono::milliseconds timeout) {
        return std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds{0});
    }

    static std::chrono::milliseconds remainingUntil(
        std::chrono::steady_clock::time_point deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        return std::max(remaining, std::chrono::milliseconds{0});
    }

    static std::optional<ErrorCode> invokeProgress(const CallOptions& options) {
        try {
            if (options.progress) {
                options.progress();
            }
        } catch (...) {
            return ErrorCode::InternalError;
        }
        return std::nullopt;
    }

    static std::optional<ErrorCode> invokeCancellation(const CallOptions& options) {
        try {
            if (options.cancelled && options.cancelled()) {
                return ErrorCode::Canceled;
            }
        } catch (...) {
            return ErrorCode::InternalError;
        }
        return std::nullopt;
    }

    static std::optional<ErrorCode> invokeProgress(const AppReadyOptions& appOptions,
                                                   const CallOptions* callOptions) {
        if (callOptions != nullptr) {
            return invokeProgress(*callOptions);
        }
        try {
            if (appOptions.progress) {
                appOptions.progress();
            }
        } catch (...) {
            return ErrorCode::InternalError;
        }
        return std::nullopt;
    }

    static std::optional<ErrorCode> invokeCancellation(const AppReadyOptions& appOptions,
                                                       const CallOptions* callOptions) {
        if (callOptions != nullptr) {
            return invokeCancellation(*callOptions);
        }
        try {
            if (appOptions.cancelled && appOptions.cancelled()) {
                return ErrorCode::Canceled;
            }
        } catch (...) {
            return ErrorCode::InternalError;
        }
        return std::nullopt;
    }

    bool normalizeRequest(RpcPayload& request, const CallOptions& options) {
        if (request.requestId == 0) {
            if (!allocateRequestId(request.requestId)) {
                return false;
            }
        } else if (!_usedRequestIds.insert(request.requestId).second) {
            return false;
        }
        (void)options;
        if (request.bodyEncoding == RpcBodyEncoding::Tlv8 && !isJsonBinaryRpcEncoding(request.encoding)) {
            request.bodyEncoding = bodyEncodingFor(request.encoding);
        }
        request.op = RpcOp::Request;
        request.meta.requestId = request.requestId;
        if (request.meta.sourceProtocol == SourceProtocol::JsonRpc && request.meta.jsonSid.empty() &&
            _appReady && !_sessionSid.empty()) {
            request.meta.jsonSid = _sessionSid;
        }
        return true;
    }

    bool allocateRequestId(std::uint32_t& requestId) {
        // _usedRequestIds is reset only when the physical transport is
        // attached/closed.  Skip zero on wrap and never recycle an id while a
        // late response can still arrive on this session.
        const auto firstCandidate = _nextRequestId == 0 ? 1U : _nextRequestId;
        auto candidate = firstCandidate;
        do {
            if (_usedRequestIds.insert(candidate).second) {
                requestId = candidate;
                _nextRequestId = candidate + 1U;
                return true;
            }
            ++candidate;
            if (candidate == 0) {
                candidate = 1U;
            }
        } while (candidate != firstCandidate);
        return false;
    }

    static RpcPayload makeErrorResponse(const RpcPayload& request, ErrorCode code) {
        RpcPayload response;
        response.encoding = request.encoding;
        response.op = RpcOp::RequestResponse;
        response.requestId = request.requestId;
        response.methodOrEventId = request.methodOrEventId;
        response.statusCode = code;
        response.bodyEncoding = request.bodyEncoding;
        response.meta = request.meta;
        return response;
    }

    static std::uint32_t processId() {
#if defined(_WIN32)
        return static_cast<std::uint32_t>(_getpid());
#else
        return static_cast<std::uint32_t>(getpid());
#endif
    }

    static std::uint32_t generateRandomSeed() {
        try {
            std::random_device random;
            const auto first = static_cast<std::uint32_t>(random());
            const auto second = static_cast<std::uint32_t>(random());
            const auto mixed = first ^ (second << 1U) ^ (second >> 1U);
            if (mixed != 0) {
                return mixed;
            }
        } catch (const std::exception&) {
        }

        static std::atomic<std::uint32_t> counter{0};
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto steady = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto threadHash =
            static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        auto seed = static_cast<std::uint32_t>(now) ^
                    static_cast<std::uint32_t>(static_cast<std::uint64_t>(now) >> 32U) ^
                    static_cast<std::uint32_t>(steady) ^
                    static_cast<std::uint32_t>(static_cast<std::uint64_t>(steady) >> 32U) ^
                    processId() ^ threadHash ^ (++counter * 0x9E3779B9U);
        if (seed == 0) {
            seed = 0xA5A5A5A5U ^ counter.load();
        }
        return seed;
    }

    ClientOptions _options;
    std::unique_ptr<ITransport> _transport;
    BasicBroker<> _broker;
    std::unique_ptr<AxtpEndpoint<BasicBroker<>>> _endpoint;
    IngressTokenProvider _ingressTokenProvider;
    MethodRegistry _registry = MethodRegistry::fromGeneratedDefaults();
    std::map<std::uint32_t, RawMethodHandler> _localHandlers;
    std::map<std::uint32_t, RawEventHandler> _eventHandlers;
    std::set<std::uint32_t> _usedRequestIds;
    std::uint32_t _nextRequestId = 1;
    std::uint16_t _nextControlId = 1;
    bool _appReady = false;
    bool _connected = false;
    bool _identifyOutstanding = false;
    std::string _sessionSid;
    std::optional<std::uint32_t> _pendingIdentifyRandomSeed;
    std::optional<std::uint32_t> _negotiatedHeartbeatIntervalMs;
    AppReadyResult _lastAppReady;
    SdkError _lastError;
};

}  // namespace axtp::sdk
