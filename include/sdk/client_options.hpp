#pragma once

#include <chrono>

#include "core/runtime/transport/transport_profile.hpp"

namespace axtp::sdk {

struct ClientOptions {
    AxtpWireMode wireMode = AxtpWireMode::FramedBinary;
    RpcEncoding defaultEncoding = RpcEncoding::Json;
    std::chrono::milliseconds connectTimeout{3000};
    std::chrono::milliseconds requestTimeout{5000};
    // Requested value is advisory; the peer's valid ACCEPT value is the
    // negotiated interval exposed by AxtpClient.
    std::chrono::milliseconds requestedHeartbeatInterval{1000};
    bool autoOpen = true;
    bool autoIdentify = true;
    bool autoLoadCapabilities = false;
};

}  // namespace axtp::sdk
