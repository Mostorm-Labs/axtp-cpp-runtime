#pragma once

#include <chrono>
#include <functional>

#include "core/protocol/model/payload.hpp"

namespace axtp::sdk {

struct CallOptions {
    std::chrono::milliseconds timeout{5000};
    RpcEncoding encoding = RpcEncoding::Json;
    bool validateSchema = false;
    bool acceptAnyResponse = false;
    std::function<bool()> cancelled;
    std::function<void()> progress;
    EndpointMetadata endpoint;
};

}  // namespace axtp::sdk
