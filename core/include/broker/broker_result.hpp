#pragma once

#include <utility>

#include "model/payload.hpp"

namespace axtp {

enum class BrokerResultType {
    RpcResponse,
    RpcError,
    Event,
    StreamData,
    StreamClose,
    Noop,
};

struct BrokerResult {
    BrokerResultType type = BrokerResultType::Noop;
    RpcPayload rpc;
    StreamPayload stream;
    ControlPayload control;

    static BrokerResult rpcResponse(RpcPayload payload) {
        BrokerResult result;
        result.type = BrokerResultType::RpcResponse;
        result.rpc = std::move(payload);
        return result;
    }

    static BrokerResult rpcError(RpcPayload payload) {
        BrokerResult result;
        result.type = BrokerResultType::RpcError;
        result.rpc = std::move(payload);
        return result;
    }

    static BrokerResult event(RpcPayload payload) {
        BrokerResult result;
        result.type = BrokerResultType::Event;
        result.rpc = std::move(payload);
        return result;
    }

    static BrokerResult streamData(StreamPayload payload) {
        BrokerResult result;
        result.type = BrokerResultType::StreamData;
        result.stream = std::move(payload);
        return result;
    }

    static BrokerResult streamClose(StreamPayload payload) {
        BrokerResult result;
        result.type = BrokerResultType::StreamClose;
        result.stream = std::move(payload);
        return result;
    }

    static BrokerResult noop() {
        return {};
    }
};

}  // namespace axtp
