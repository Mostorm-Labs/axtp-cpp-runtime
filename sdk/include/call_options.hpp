#pragma once

#include <chrono>

#include "model/payload.hpp"

namespace axtp::sdk {

struct CallOptions {
    std::chrono::milliseconds timeout{5000};
    RpcEncoding encoding = RpcEncoding::Json;
    bool validateSchema = false;
};

}  // namespace axtp::sdk
