#pragma once

#include <utility>

#include "runtime/broker/broker_task.hpp"
#include "runtime/broker/business_router.hpp"

namespace axtp {

class BusinessExecutor {
public:
    RpcPayload executeRpcRequest(const BusinessRouter& router, const BrokerTask& task) const {
        return router.handleRpcRequest(task.rpc);
    }
};

}  // namespace axtp
