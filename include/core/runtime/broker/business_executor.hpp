#pragma once

#include <utility>

#include "core/runtime/broker/broker_task.hpp"
#include "core/runtime/broker/business_router.hpp"

namespace axtp {

class BusinessExecutor {
public:
    RpcDispatch executeRpcRequest(const BusinessRouter& router, const BrokerTask& task) const {
        return router.dispatchRpcRequest(task.rpc);
    }

    void completeRpcRequest(RpcPayload& response, const RpcResponseData& data) const {
        BusinessRouter::applyResponseData(response, data);
    }
};

}  // namespace axtp
