#pragma once

#include <utility>

#include "core/protocol/model/payload.hpp"

namespace axtp::sdk {

class StreamClient {
public:
    void onStreamData(StreamPayload payload) {
        _lastPayload = std::move(payload);
    }

    const StreamPayload& lastPayload() const {
        return _lastPayload;
    }

private:
    StreamPayload _lastPayload;
};

}  // namespace axtp::sdk
