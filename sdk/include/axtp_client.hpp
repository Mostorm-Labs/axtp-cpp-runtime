#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "axtp.hpp"
#include "generated/method_traits.h"
#include "generated/schema_codec.h"

#include "call_options.hpp"
#include "client_options.hpp"
#include "endpoints.hpp"
#include "sdk_error.hpp"

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

    void registerMethod(std::uint32_t methodId, RawMethodHandler handler) {
        _localHandlers[methodId] = std::move(handler);
    }

    void registerEventHandler(std::uint32_t eventId, RawEventHandler handler) {
        _eventHandlers[eventId] = std::move(handler);
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
        _endpoint->sendRpcRequest(request);

        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            poll();
            if (auto response = _endpoint->tryTakeRpcResponse(request.requestId)) {
                return *response;
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
        options.encoding = RpcEncoding::Tlv;
        return callRaw(methodId, RpcEncoding::Tlv, std::move(tlvBody), options);
    }

    Bytes callRawBytes(std::uint32_t methodId, Bytes body, CallOptions options = {}) {
        options.encoding = RpcEncoding::Raw;
        return callRaw(methodId, RpcEncoding::Raw, std::move(body), options);
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
        payload.meta.sourceProtocol = _options.wireMode == AxtpWireMode::WebSocketJsonRpc
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
        if (encoding == RpcEncoding::Tlv || encoding == RpcEncoding::Binary) {
            return RpcBodyEncoding::Tlv8;
        }
        return RpcBodyEncoding::RawBytes;
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
        payload.meta.sourceProtocol = _options.wireMode == AxtpWireMode::WebSocketJsonRpc
                                          ? SourceProtocol::JsonRpc
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
        if (request.bodyEncoding == RpcBodyEncoding::Tlv8 && request.encoding != RpcEncoding::Tlv &&
            request.encoding != RpcEncoding::Binary) {
            request.bodyEncoding = bodyEncodingFor(request.encoding);
        }
        request.op = RpcOp::Request;
        request.meta.requestId = request.requestId;
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

    ClientOptions _options;
    std::unique_ptr<ITransport> _transport;
    BasicBroker<> _broker;
    std::unique_ptr<AxtpEndpoint<BasicBroker<>>> _endpoint;
    MethodRegistry _registry = MethodRegistry::fromGeneratedDefaults();
    std::map<std::uint32_t, RawMethodHandler> _localHandlers;
    std::map<std::uint32_t, RawEventHandler> _eventHandlers;
    std::uint32_t _nextRequestId = 1;
    bool _connected = false;
    SdkError _lastError;
};

}  // namespace axtp::sdk
