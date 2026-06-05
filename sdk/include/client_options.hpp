#pragma once

#include <chrono>

#include "transport/transport_profile.hpp"

namespace axtp::sdk {

struct ClientOptions {
    AxtpWireMode wireMode = AxtpWireMode::FramedBinary;
    RpcEncoding defaultEncoding = RpcEncoding::Json;
    std::chrono::milliseconds connectTimeout{3000};
    std::chrono::milliseconds requestTimeout{5000};
    bool autoOpen = true;
    bool autoIdentify = true;
    bool autoLoadCapabilities = false;
};

}  // namespace axtp::sdk
