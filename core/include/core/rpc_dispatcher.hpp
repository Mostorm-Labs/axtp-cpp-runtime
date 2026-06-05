#pragma once

#include <functional>
#include <map>
#include <optional>
#include <utility>

#include "model/payload.hpp"

namespace axtp {

class RpcDispatcher {
public:
    using Handler = std::function<Bytes(const RpcPayload&)>;

    void registerHandler(std::uint16_t methodId, Handler handler) {
        _handlers[methodId] = std::move(handler);
    }

    std::optional<RpcPayload> handle(const RpcPayload& request) const {
        if (request.op != RpcOp::Request) {
            return std::nullopt;
        }

        RpcPayload response;
        response.encoding = request.encoding;
        response.op = RpcOp::RequestResponse;
        response.requestId = request.requestId;
        response.methodOrEventId = request.methodOrEventId;
        response.bodyEncoding = request.bodyEncoding;
        response.meta = request.meta;
        response.meta.requestId = request.requestId;

        auto it = _handlers.find(request.methodOrEventId);
        if (it == _handlers.end()) {
            response.statusCode = ErrorCode::RpcMethodNotFound;
            return response;
        }

        response.statusCode = ErrorCode::Success;
        response.body = it->second(request);
        return response;
    }

private:
    std::map<std::uint16_t, Handler> _handlers;
};

}  // namespace axtp
