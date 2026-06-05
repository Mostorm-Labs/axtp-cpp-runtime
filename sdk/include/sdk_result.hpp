#pragma once

#include <utility>

#include "sdk_error.hpp"

namespace axtp::sdk {

template <typename T>
struct SdkResult {
    SdkError error;
    T value{};

    bool ok() const {
        return error.ok();
    }

    static SdkResult success(T value) {
        return SdkResult{SdkError::success(), std::move(value)};
    }

    static SdkResult failure(ErrorCode code, std::string message = {}) {
        return SdkResult{SdkError::failure(code, std::move(message)), T{}};
    }
};

}  // namespace axtp::sdk
