#pragma once

#include <cstdint>
#include <functional>

#include "protocol/model/payload.hpp"

namespace axtp::sdk {

struct EventSubscription {
    std::uint16_t eventId = 0;
    std::function<void(const RpcPayload&)> handler;
};

}  // namespace axtp::sdk
