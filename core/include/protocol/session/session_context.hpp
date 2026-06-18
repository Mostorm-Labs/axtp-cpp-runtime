#pragma once

#include <utility>

#include "protocol/model/payload.hpp"
#include "protocol/model/protocol_types.hpp"
#include "runtime/transport/transport_profile.hpp"

namespace axtp {

struct SessionContext {
    std::uint32_t sessionId = 0;
    TransportProfile transportProfile;
    RpcEncoding selectedEncoding = jsonBinaryRpcEncoding();
};

class IProtocolOutbound {
public:
    virtual ~IProtocolOutbound() = default;
    virtual void sendRpc(RpcPayload payload) = 0;
};

}  // namespace axtp
