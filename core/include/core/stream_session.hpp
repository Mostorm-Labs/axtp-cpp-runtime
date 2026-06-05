#pragma once

#include <optional>
#include <utility>

#include "model/payload.hpp"

namespace axtp {

class StreamSession {
public:
    void handle(StreamPayload payload) {
        _lastStream = std::move(payload);
    }

    const std::optional<StreamPayload>& lastStream() const {
        return _lastStream;
    }

private:
    std::optional<StreamPayload> _lastStream;
};

}  // namespace axtp
