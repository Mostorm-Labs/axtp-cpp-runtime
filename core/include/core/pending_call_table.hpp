#pragma once

#include <map>
#include <optional>
#include <utility>

#include "model/payload.hpp"

namespace axtp {

class PendingCallTable {
public:
    void expect(std::uint32_t requestId) {
        _pending[requestId] = {};
    }

    void resolve(std::uint32_t requestId, RpcPayload payload) {
        _resolved[requestId] = std::move(payload);
        _pending.erase(requestId);
    }

    bool isPending(std::uint32_t requestId) const {
        return _pending.find(requestId) != _pending.end();
    }

    std::optional<RpcPayload> tryTakeResolved(std::uint32_t requestId) {
        auto it = _resolved.find(requestId);
        if (it == _resolved.end()) {
            return std::nullopt;
        }
        auto payload = std::move(it->second);
        _resolved.erase(it);
        return payload;
    }

private:
    std::map<std::uint32_t, RpcPayload> _pending;
    std::map<std::uint32_t, RpcPayload> _resolved;
};

}  // namespace axtp
