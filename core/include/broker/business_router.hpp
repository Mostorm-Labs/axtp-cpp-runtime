#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "generated/method_registry.h"
#include "model/payload.hpp"

namespace axtp {

struct RpcContext {
    std::uint32_t sessionId = 0;
    std::uint32_t requestId = 0;
    std::uint32_t methodId = 0;
    std::string methodName;
    RpcEncoding encoding = RpcEncoding::Json;
    SourceProtocol sourceProtocol = SourceProtocol::AxtpV1;
};

struct RpcRequestView {
    std::uint32_t methodId = 0;
    std::string methodName;
    std::uint32_t requestId = 0;
    RpcEncoding encoding = RpcEncoding::Json;
    Bytes body;
};

struct RpcResponseData {
    RpcEncoding encoding = RpcEncoding::Json;
    Bytes body;
    bool overrideEncoding = false;
};

using RawRpcHandler = std::function<RpcResponseData(const RpcContext&, const RpcRequestView&)>;
using JsonRpcHandler = std::function<std::string(const RpcContext&, std::string_view)>;
using TlvRpcHandler = std::function<Bytes(const RpcContext&, const Bytes&)>;

class BusinessRouter {
public:
    using Handler = std::function<Bytes(const RpcPayload&)>;

    MethodRegistry& registry() {
        return _registry;
    }

    const MethodRegistry& registry() const {
        return _registry;
    }

    void registerMethod(std::uint32_t methodId, Handler handler) {
        registerRawMethod(methodId,
                          [handler = std::move(handler)](const RpcContext& context,
                                                         const RpcRequestView& request) {
                              RpcPayload payload;
                              payload.encoding = request.encoding;
                              payload.op = RpcOp::Request;
                              payload.requestId = request.requestId;
                              payload.methodOrEventId = request.methodId;
                              payload.meta.sourceProtocol = context.sourceProtocol;
                              payload.meta.sessionId = context.sessionId;
                              payload.meta.requestId = context.requestId;
                              payload.meta.jsonMethodOrEventName = context.methodName;
                              payload.body = request.body;
                              RpcResponseData response;
                              response.body = handler(payload);
                              return response;
                          });
    }

    void registerRawMethod(std::uint32_t methodId, RawRpcHandler handler) {
        _handlers[methodId] = std::move(handler);
    }

    void registerJsonMethod(std::uint32_t methodId, JsonRpcHandler handler) {
        registerRawMethod(methodId,
                          [handler = std::move(handler)](const RpcContext& context,
                                                         const RpcRequestView& request) {
                              const std::string params(request.body.begin(), request.body.end());
                              const auto result = handler(context, params);
                              RpcResponseData response;
                              response.encoding = RpcEncoding::Json;
                              response.body.assign(result.begin(), result.end());
                              response.overrideEncoding = true;
                              return response;
                          });
    }

    void registerJsonMethod(std::string_view methodName, JsonRpcHandler handler) {
        const auto methodId = _registry.findMethodId(methodName);
        if (!methodId.has_value()) {
            return;
        }
        registerJsonMethod(*methodId, std::move(handler));
    }

    void registerTlvMethod(std::uint32_t methodId, TlvRpcHandler handler) {
        registerRawMethod(methodId,
                          [handler = std::move(handler)](const RpcContext& context,
                                                         const RpcRequestView& request) {
                              RpcResponseData response;
                              response.encoding = RpcEncoding::Tlv;
                              response.body = handler(context, request.body);
                              response.overrideEncoding = true;
                              return response;
                          });
    }

    void registerTlvMethod(std::string_view methodName, TlvRpcHandler handler) {
        const auto methodId = _registry.findMethodId(methodName);
        if (!methodId.has_value()) {
            return;
        }
        registerTlvMethod(*methodId, std::move(handler));
    }

    RpcPayload handleRpcRequest(const RpcPayload& request) const {
        RpcPayload response;
        response.encoding = request.encoding;
        response.op = RpcOp::RequestResponse;
        response.requestId = request.requestId;
        response.methodOrEventId = request.methodOrEventId;
        response.statusCode = ErrorCode::Success;
        response.bodyEncoding = request.bodyEncoding;
        response.meta = request.meta;

        auto it = _handlers.find(request.methodOrEventId);
        if (it == _handlers.end()) {
            response.statusCode = ErrorCode::RpcMethodNotFound;
            return response;
        }

        const auto methodName = _registry.findMethodName(request.methodOrEventId);
        RpcContext context;
        context.sessionId = request.meta.sessionId;
        context.requestId = request.requestId;
        context.methodId = request.methodOrEventId;
        context.methodName = methodName.has_value() ? std::string(*methodName) : std::string();
        context.encoding = request.encoding;
        context.sourceProtocol = request.meta.sourceProtocol;

        RpcRequestView view;
        view.methodId = request.methodOrEventId;
        view.methodName = context.methodName;
        view.requestId = request.requestId;
        view.encoding = request.encoding;
        view.body = request.body;

        const auto data = it->second(context, view);
        if (data.overrideEncoding) {
            response.encoding = data.encoding;
            response.bodyEncoding =
                data.encoding == RpcEncoding::Tlv || data.encoding == RpcEncoding::Binary
                    ? RpcBodyEncoding::Tlv8
                    : RpcBodyEncoding::RawBytes;
        }
        response.body = data.body;
        return response;
    }

private:
    MethodRegistry _registry = MethodRegistry::fromGeneratedDefaults();
    std::map<std::uint32_t, RawRpcHandler> _handlers;
};

}  // namespace axtp
