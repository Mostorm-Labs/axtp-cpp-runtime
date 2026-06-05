#pragma once

#include <string>
#include <utility>

#include "model/error.hpp"

namespace axtp {

struct Status {
    bool ok = true;
    ErrorCode code = ErrorCode::Success;
    std::string message;

    static Status success() {
        return {};
    }

    static Status failure(ErrorCode code, std::string message = {}) {
        return Status{false, code, std::move(message)};
    }
};

template <typename T>
struct Result {
    Status status;
    T value{};

    bool ok() const {
        return status.ok;
    }

    static Result<T> success(T value) {
        return Result<T>{Status::success(), std::move(value)};
    }

    static Result<T> failure(ErrorCode code, std::string message = {}) {
        return Result<T>{Status::failure(code, std::move(message)), T{}};
    }
};

}  // namespace axtp
