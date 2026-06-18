#pragma once

#include "runtime/broker/broker_task.hpp"

namespace axtp {

class MiddlewareChain {
public:
    bool before(BrokerTask&) const {
        return true;
    }
};

}  // namespace axtp
