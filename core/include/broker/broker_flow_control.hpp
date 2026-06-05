#pragma once

#include "broker/broker_task.hpp"

namespace axtp {

class BrokerFlowControl {
public:
    bool allow(const BrokerTask&) const {
        return true;
    }
};

}  // namespace axtp
