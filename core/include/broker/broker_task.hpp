#pragma once

#include <utility>

#include "broker/broker_context.hpp"
#include "core/core_event.hpp"
#include "model/payload.hpp"

namespace axtp {

struct BrokerTask {
    BrokerTaskType type = BrokerTaskType::RpcRequest;
    BrokerPriority priority = BrokerPriority::Normal;
    BrokerContext context;
    RpcPayload rpc;
    StreamPayload stream;
    ControlPayload control;

    static BrokerTask fromCoreEvent(CoreEvent event) {
        BrokerTask task;
        switch (event.type) {
        case CoreEventType::RpcRequest:
            task.type = BrokerTaskType::RpcRequest;
            task.rpc = std::move(event.rpc);
            task.context.sessionId = task.rpc.meta.sessionId;
            task.context.requestId = task.rpc.requestId;
            task.context.methodOrEventId = task.rpc.methodOrEventId;
            task.context.encoding = task.rpc.encoding;
            task.context.sourceProtocol = task.rpc.meta.sourceProtocol;
            break;
        case CoreEventType::RpcEvent:
            task.type = BrokerTaskType::RpcEvent;
            task.rpc = std::move(event.rpc);
            task.context.sessionId = task.rpc.meta.sessionId;
            task.context.methodOrEventId = task.rpc.methodOrEventId;
            task.context.encoding = task.rpc.encoding;
            task.context.sourceProtocol = task.rpc.meta.sourceProtocol;
            break;
        case CoreEventType::StreamOpen:
            task.type = BrokerTaskType::StreamOpen;
            task.stream = std::move(event.stream);
            task.context.sessionId = task.stream.meta.sessionId;
            break;
        case CoreEventType::StreamData:
            task.type = BrokerTaskType::StreamData;
            task.stream = std::move(event.stream);
            task.context.sessionId = task.stream.meta.sessionId;
            break;
        case CoreEventType::StreamClose:
            task.type = BrokerTaskType::StreamClose;
            task.stream = std::move(event.stream);
            task.context.sessionId = task.stream.meta.sessionId;
            break;
        case CoreEventType::ControlNotice:
            task.type = BrokerTaskType::ControlNotice;
            task.control = std::move(event.control);
            task.context.sessionId = task.control.meta.sessionId;
            break;
        case CoreEventType::ProtocolError:
            task.type = BrokerTaskType::ControlNotice;
            break;
        }
        return task;
    }
};

}  // namespace axtp
