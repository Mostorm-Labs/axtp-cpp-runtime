#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
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

    explicit AxtpClient(ClientOptions options = {})
        : _options(options)
        , _endpoint(std::make_unique<AxtpEndpoint<BasicBroker<>>>(_broker)) {}

    ~AxtpClient() {
        close();
    }

    void attachTransport(std::unique_ptr<ITransport> transport) {
        close();
        _transport = std::move(transport);
        _endpoint = std::make_unique<AxtpEndpoint<BasicBroker<>>>(_broker);
        _appReady = false;
        _sessionSid.clear();
        if (_transport != nullptr) {
            _endpoint->attachTransport(*_transport);
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
    }

    bool isConnected() const {
        return _connected;
    }

    const SdkError& lastError() const {
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
        const auto deadline = std::chrono::steady_clock::now() + options.timeout;

        if (profile.wireMode == AxtpWireMode::FramedBinary && !options.skipControlOpen) {
            result.stage = "control-open";
            const auto controlId = _nextControlId++;
            emitTrace("control-open", "send", ErrorCode::Success, controlId);
            _endpoint->sendControlOpen(controlId);
            emitTrace("control-accept", "wait", ErrorCode::Success, controlId);
            while (std::chrono::steady_clock::now() < deadline) {
                poll();
                if (auto accept = _endpoint->tryTakeControlNotice(ControlOpcode::Accept)) {
                    emitTrace("control-accept",
                              "receive",
                              accept->statusCode,
                              accept->controlId,
                              "",
                              "",
                              accept->controlId == controlId ? "controlId matched"
                                                             : "controlId mismatch");
                    if (accept->controlId == controlId &&
                        accept->statusCode == ErrorCode::Success &&
                        _endpoint->core().controlSessionOpen()) {
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
        while (std::chrono::steady_clock::now() < deadline) {
            poll();
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

        emitTrace("identified", "wait");
        while (std::chrono::steady_clock::now() < deadline) {
            poll();
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
                if (!identified->meta.jsonSid.empty()) {
                    _sessionSid = identified->meta.jsonSid;
                    _appReady = true;
                    result.ok = true;
                    result.statusCode = ErrorCode::Success;
                    result.stage = "app-ready";
                    result.sid = _sessionSid;
                    emitTrace("app-ready", "ready", ErrorCode::Success, 0, result.sid, "", "", true);
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
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        result.statusCode = ErrorCode::RpcResponseTimeout;
        emitTrace("identified", "timeout", result.statusCode, 0, "", "", "", true);
        _lastAppReady = result;
        _lastError = SdkError::failure(result.statusCode, "identified timeout");
        return result;
    }

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
        normalizeRequest(request, options);

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
            response.body = local->second(request);
            return response;
        }

        if (_transport == nullptr || _endpoint == nullptr) {
            return makeErrorResponse(request, ErrorCode::Unavailable);
        }

        const auto profile = _transport->profile();
        _endpoint->core().configure(profile);
        if (_options.autoIdentify && !_appReady) {
            AppReadyOptions appOptions;
            appOptions.timeout = options.timeout;
            const auto ready = ensureAppReady(appOptions);
            if (!ready.ok) {
                return makeErrorResponse(request, ready.statusCode);
            }
            if (request.meta.sourceProtocol == SourceProtocol::JsonRpc &&
                request.meta.jsonSid.empty() && !_sessionSid.empty()) {
                request.meta.jsonSid = _sessionSid;
            }
        }
        _endpoint->sendRpcRequest(request);

        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            poll();
            if (auto response = _endpoint->tryTakeRpcResponse(request.requestId)) {
                return *response;
            }
            if (options.acceptAnyResponse) {
                if (auto response = _endpoint->tryTakeAnyRpcResponse()) {
                    return *response;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return makeErrorResponse(request, ErrorCode::RpcResponseTimeout);
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
            it->second(eventPayload);
        }
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

    void normalizeRequest(RpcPayload& request, const CallOptions& options) {
        if (request.requestId == 0) {
            request.requestId = _nextRequestId++;
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
    MethodRegistry _registry = MethodRegistry::fromGeneratedDefaults();
    std::map<std::uint32_t, RawMethodHandler> _localHandlers;
    std::map<std::uint32_t, RawEventHandler> _eventHandlers;
    std::uint32_t _nextRequestId = 1;
    std::uint16_t _nextControlId = 1;
    bool _appReady = false;
    bool _connected = false;
    std::string _sessionSid;
    AppReadyResult _lastAppReady;
    SdkError _lastError;
};

}  // namespace axtp::sdk
