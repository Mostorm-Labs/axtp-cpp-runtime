#pragma once

#include "model/payload.hpp"

namespace axtp {

class IPayloadSink {
public:
    virtual ~IPayloadSink() = default;
    virtual void onControl(ControlPayload payload) = 0;
    virtual void onRpc(RpcPayload payload) = 0;
    virtual void onStream(StreamPayload payload) = 0;
};

}  // namespace axtp
