#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/protocol/model/error.hpp"

namespace axtp::sdk {

struct AppReadyTraceEvent {
    std::string stage;
    std::string action;
    ErrorCode statusCode = ErrorCode::Success;
    std::uint16_t controlId = 0;
    bool hasRandomSeed = false;
    std::uint32_t randomSeed = 0;
    std::string sid;
    std::string bodyText;
    std::string detail;
};

struct AppReadyOptions {
    std::chrono::milliseconds timeout{5000};
    std::string eventMasks;
    std::optional<std::uint32_t> randomSeed;
    std::function<void(const AppReadyTraceEvent&)> trace;
    bool skipControlOpen = false;
    // Standalone setup is also a synchronous wait.  These hooks mirror the
    // call options used by automatic setup so callers can keep pumping their
    // owner loop and stop an explicit ensureAppReady() without a second
    // timeout domain.
    std::function<bool()> cancelled;
    std::function<void()> progress;
};

struct AppReadyResult {
    bool ok = false;
    ErrorCode statusCode = ErrorCode::Success;
    std::string stage;
    std::string sid;
    bool hasRandomSeed = false;
    std::uint32_t randomSeed = 0;
};

}  // namespace axtp::sdk
