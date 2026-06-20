#pragma once

#include <utility>

#include "core/protocol/model/payload.hpp"
#include "core/protocol/model/protocol_types.hpp"
#include "core/runtime/transport/transport_profile.hpp"

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
