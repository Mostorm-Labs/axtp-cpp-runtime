#pragma once

#include <string>
#include <utility>

#include "model/error.hpp"

namespace axtp::sdk {

struct SdkError {
    ErrorCode code = ErrorCode::Success;
    std::string message;

    bool ok() const {
        return code == ErrorCode::Success;
    }

    static SdkError success() {
        return {};
    }

    static SdkError failure(ErrorCode code, std::string message = {}) {
        return SdkError{code, std::move(message)};
    }
};

}  // namespace axtp::sdk
