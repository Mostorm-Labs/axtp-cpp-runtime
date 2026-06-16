#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "model/error.hpp"

namespace axtp::sdk {

struct AppReadyOptions {
    std::chrono::milliseconds timeout{5000};
    std::string eventMasks;
    std::optional<std::uint32_t> clientSeed;
    bool skipControlOpen = false;
};

struct AppReadyResult {
    bool ok = false;
    ErrorCode statusCode = ErrorCode::Success;
    std::string stage;
    std::string sid;
    std::uint32_t clientSeed = 0;
};

}  // namespace axtp::sdk
