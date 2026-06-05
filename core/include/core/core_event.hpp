#pragma once

#include <utility>

#include "model/payload.hpp"

namespace axtp {

enum class CoreEventType {
    RpcRequest,
    RpcEvent,
    StreamOpen,
    StreamData,
    StreamClose,
    ControlNotice,
    ProtocolError,
};

struct CoreEvent {
    CoreEventType type = CoreEventType::ProtocolError;
    RpcPayload rpc;
    StreamPayload stream;
    ControlPayload control;
    ErrorCode error = ErrorCode::Success;

    static CoreEvent rpcRequest(RpcPayload payload) {
        CoreEvent event;
        event.type = CoreEventType::RpcRequest;
        event.rpc = std::move(payload);
        return event;
    }

    static CoreEvent rpcEvent(RpcPayload payload) {
        CoreEvent event;
        event.type = CoreEventType::RpcEvent;
        event.rpc = std::move(payload);
        return event;
    }

    static CoreEvent streamData(StreamPayload payload) {
        CoreEvent event;
        event.type = CoreEventType::StreamData;
        event.stream = std::move(payload);
        return event;
    }

    static CoreEvent controlNotice(ControlPayload payload) {
        CoreEvent event;
        event.type = CoreEventType::ControlNotice;
        event.control = std::move(payload);
        return event;
    }

    static CoreEvent protocolError(ErrorCode value) {
        CoreEvent event;
        event.type = CoreEventType::ProtocolError;
        event.error = value;
        return event;
    }
};

}  // namespace axtp
