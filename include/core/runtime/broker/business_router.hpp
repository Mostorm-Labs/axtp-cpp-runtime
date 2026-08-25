#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/protocol/generated/method_registry.h"
#include "core/protocol/model/payload.hpp"

namespace axtp {

struct RpcContext {
    std::uint32_t sessionId = 0;
    std::uint32_t requestId = 0;
    std::uint32_t methodId = 0;
    std::string methodName;
    RpcEncoding encoding = RpcEncoding::Json;
    SourceProtocol sourceProtocol = SourceProtocol::AxtpV1;
    EndpointMetadata endpoint;
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
    ErrorCode statusCode = ErrorCode::Success;
    bool overrideStatus = false;
};

using RawRpcHandler = std::function<RpcResponseData(const RpcContext&, const RpcRequestView&)>;
using JsonRpcHandler = std::function<std::string(const RpcContext&, std::string_view)>;
using TlvRpcHandler = std::function<Bytes(const RpcContext&, const Bytes&)>;
using DeferredRpcPoll = std::function<std::optional<RpcResponseData>()>;
using DeferredRawRpcHandler =
    std::function<DeferredRpcPoll(const RpcContext&, const RpcRequestView&)>;

struct RpcDispatch {
    RpcPayload response;
    DeferredRpcPoll deferredPoll;
};

class BusinessRouter {
public:
    using Handler = std::function<Bytes(const RpcPayload&)>;
    using RequestValidator = std::function<ErrorCode(const RpcPayload&)>;

    void registerRequestValidator(RequestValidator validator) {
        _validators.push_back(std::move(validator));
    }

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
                              payload.meta.endpoint = context.endpoint;
                              payload.body = request.body;
                              RpcResponseData response;
                              response.body = handler(payload);
                              return response;
                          });
    }

    void registerRawMethod(std::uint32_t methodId, RawRpcHandler handler) {
        _deferredHandlers.erase(methodId);
        _handlers[methodId] = std::move(handler);
    }

    void registerDeferredRawMethod(std::uint32_t methodId, DeferredRawRpcHandler handler) {
        _handlers.erase(methodId);
        _deferredHandlers[methodId] = std::move(handler);
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
                              response.encoding = jsonBinaryRpcEncoding();
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
        auto dispatch = dispatchRpcRequest(request);
        if (dispatch.deferredPoll) {
            dispatch.response.statusCode = ErrorCode::InternalError;
        }
        return dispatch.response;
    }

    RpcDispatch dispatchRpcRequest(const RpcPayload& request) const {
        RpcPayload response;
        response.encoding = request.encoding;
        response.op = RpcOp::RequestResponse;
        response.requestId = request.requestId;
        response.methodOrEventId = request.methodOrEventId;
        response.statusCode = ErrorCode::Success;
        response.bodyEncoding = request.bodyEncoding;
        response.meta = request.meta;
        response.meta.endpoint = responseEndpointMetadata(request.meta.endpoint);

        const auto handler = _handlers.find(request.methodOrEventId);
        const auto deferredHandler = _deferredHandlers.find(request.methodOrEventId);
        if (handler == _handlers.end() && deferredHandler == _deferredHandlers.end()) {
            response.statusCode = _registry.findMethodName(request.methodOrEventId).has_value()
                                      ? ErrorCode::NotSupported
                                      : ErrorCode::RpcMethodNotFound;
            return RpcDispatch{std::move(response), {}};
        }

        for (const auto& validator : _validators) {
            if (const auto validation = validator(request); validation != ErrorCode::Success) {
                response.statusCode = validation;
                return RpcDispatch{std::move(response), {}};
            }
        }

        const auto methodName = _registry.findMethodName(request.methodOrEventId);
        RpcContext context;
        context.sessionId = request.meta.sessionId;
        context.requestId = request.requestId;
        context.methodId = request.methodOrEventId;
        context.methodName = methodName.has_value() ? std::string(*methodName) : std::string();
        context.encoding = request.encoding;
        context.sourceProtocol = request.meta.sourceProtocol;
        context.endpoint = request.meta.endpoint;

        RpcRequestView view;
        view.methodId = request.methodOrEventId;
        view.methodName = context.methodName;
        view.requestId = request.requestId;
        view.encoding = request.encoding;
        view.body = request.body;

        if (handler != _handlers.end()) {
            applyResponseData(response, handler->second(context, view));
            return RpcDispatch{std::move(response), {}};
        }

        try {
            auto deferredPoll = deferredHandler->second(context, view);
            if (!deferredPoll) {
                response.statusCode = ErrorCode::InternalError;
            }
            return RpcDispatch{std::move(response), std::move(deferredPoll)};
        } catch (...) {
            response.statusCode = ErrorCode::InternalError;
            return RpcDispatch{std::move(response), {}};
        }
    }

    static void applyResponseData(RpcPayload& response, const RpcResponseData& data) {
        if (data.overrideEncoding) {
            response.encoding = data.encoding;
            response.bodyEncoding = bodyEncodingForRpcEncoding(data.encoding);
        }
        if (data.overrideStatus) {
            response.statusCode = data.statusCode;
        }
        response.body = data.body;
    }

private:
    MethodRegistry _registry = MethodRegistry::fromGeneratedDefaults();
    std::map<std::uint32_t, RawRpcHandler> _handlers;
    std::map<std::uint32_t, DeferredRawRpcHandler> _deferredHandlers;
    std::vector<RequestValidator> _validators;
};

}  // namespace axtp
