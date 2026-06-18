#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <queue>
#include <string_view>
#include <utility>

#include "runtime/broker/broker_flow_control.hpp"
#include "runtime/broker/broker_result.hpp"
#include "runtime/broker/broker_task.hpp"
#include "runtime/broker/business_executor.hpp"
#include "runtime/broker/business_router.hpp"
#include "runtime/broker/middleware_chain.hpp"

namespace axtp {

template <std::size_t QueueSize = 32>
class BasicBroker {
public:
    using EventHandler = std::function<void(const BrokerContext&, const RpcPayload&)>;
    using StreamHandler = std::function<void(const BrokerContext&, const StreamPayload&)>;

    MethodRegistry& registry() {
        return _router.registry();
    }

    const MethodRegistry& registry() const {
        return _router.registry();
    }

    void submit(BrokerTask task) {
        _tasks.push(std::move(task));
    }

    void poll(std::size_t maxTasks = 8) {
        std::size_t processed = 0;
        while (!_tasks.empty() && processed < maxTasks) {
            auto task = std::move(_tasks.front());
            _tasks.pop();
            ++processed;
            if (!_middleware.before(task) || !_flowControl.allow(task)) {
                continue;
            }
            if (task.type == BrokerTaskType::RpcRequest) {
                auto response = _executor.executeRpcRequest(_router, task);
                if (response.statusCode == ErrorCode::Success) {
                    _results.push(BrokerResult::rpcResponse(std::move(response)));
                } else {
                    _results.push(BrokerResult::rpcError(std::move(response)));
                }
                continue;
            }
            if (task.type == BrokerTaskType::RpcEvent) {
                if (_eventHandler) {
                    _eventHandler(task.context, task.rpc);
                }
                continue;
            }
            if (task.type == BrokerTaskType::StreamData) {
                if (_streamHandler) {
                    _streamHandler(task.context, task.stream);
                }
                continue;
            }
            if (task.type == BrokerTaskType::StreamClose) {
                if (_streamHandler) {
                    _streamHandler(task.context, task.stream);
                }
            }
        }
    }

    std::optional<BrokerResult> pollResult() {
        if (_results.empty()) {
            return std::nullopt;
        }
        auto result = std::move(_results.front());
        _results.pop();
        return result;
    }

    template <typename Handler>
    void registerMethod(std::uint32_t methodId, Handler&& handler) {
        _router.registerMethod(methodId, std::forward<Handler>(handler));
    }

    void registerRawMethod(std::uint32_t methodId, RawRpcHandler handler) {
        _router.registerRawMethod(methodId, std::move(handler));
    }

    void registerJsonMethod(std::uint32_t methodId, JsonRpcHandler handler) {
        _router.registerJsonMethod(methodId, std::move(handler));
    }

    void registerJsonMethod(std::string_view methodName, JsonRpcHandler handler) {
        _router.registerJsonMethod(methodName, std::move(handler));
    }

    void registerTlvMethod(std::uint32_t methodId, TlvRpcHandler handler) {
        _router.registerTlvMethod(methodId, std::move(handler));
    }

    void registerTlvMethod(std::string_view methodName, TlvRpcHandler handler) {
        _router.registerTlvMethod(methodName, std::move(handler));
    }

    void registerEventHandler(EventHandler handler) {
        _eventHandler = std::move(handler);
    }

    void registerStreamHandler(StreamHandler handler) {
        _streamHandler = std::move(handler);
    }

    std::size_t queuedTaskCount() const {
        return _tasks.size();
    }

    std::size_t queuedResultCount() const {
        return _results.size();
    }

private:
    std::queue<BrokerTask> _tasks;
    std::queue<BrokerResult> _results;
    MiddlewareChain _middleware;
    BrokerFlowControl _flowControl;
    BusinessRouter _router;
    BusinessExecutor _executor;
    EventHandler _eventHandler;
    StreamHandler _streamHandler;
};

}  // namespace axtp
