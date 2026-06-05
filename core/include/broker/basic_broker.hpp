#pragma once

#include <cstddef>
#include <optional>
#include <queue>
#include <string_view>
#include <utility>

#include "broker/broker_flow_control.hpp"
#include "broker/broker_result.hpp"
#include "broker/broker_task.hpp"
#include "broker/business_executor.hpp"
#include "broker/business_router.hpp"
#include "broker/middleware_chain.hpp"

namespace axtp {

template <std::size_t QueueSize = 32>
class BasicBroker {
public:
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
                _results.push(BrokerResult::event(std::move(task.rpc)));
                continue;
            }
            if (task.type == BrokerTaskType::StreamData) {
                _results.push(BrokerResult::streamData(std::move(task.stream)));
                continue;
            }
            if (task.type == BrokerTaskType::StreamClose) {
                _results.push(BrokerResult::streamClose(std::move(task.stream)));
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
};

}  // namespace axtp
