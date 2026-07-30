#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "core/protocol/model/payload.hpp"

namespace axtp {

class PendingCallTable {
public:
    // Returns false when a non-zero id was retired earlier in this physical
    // session.  Retired ids intentionally remain retired until reset(): a
    // delayed response is allowed to arrive more than once and must never be
    // mistaken for a response to a later call.
    bool expect(std::uint32_t requestId) {
        if (requestId != 0 && _retired.find(requestId) != _retired.end()) {
            return false;
        }
        // A non-zero request id may not be claimed twice while the first
        // request is still in flight. Overwriting the pending entry would
        // otherwise let the first response complete the second logical call.
        if (requestId != 0 && _pending.find(requestId) != _pending.end()) {
            return false;
        }
        _resolved.erase(requestId);
        _pending[requestId] = {};
        return true;
    }

    void resolve(std::uint32_t requestId, RpcPayload payload) {
        if (requestId != 0 && _retired.find(requestId) != _retired.end()) {
            return;
        }
        // Request id zero is the legacy response channel.  It is the only
        // response that can be accepted without a matching pending request.
        // All unknown non-zero ids are stale or unsolicited and are dropped.
        if (requestId != 0 && !isPending(requestId)) {
            return;
        }
        _resolved[requestId] = std::move(payload);
        _pending.erase(requestId);
        // A request id is single-use for the lifetime of a physical
        // session, including after a successful response.  Keeping it in the
        // retired set makes the invariant hold for direct Endpoint/Core
        // users too (the SDK additionally tracks ids before sending).  A
        // duplicate or delayed response can therefore never satisfy a later
        // request that happens to reuse the same id.
        if (requestId != 0) {
            _retired.insert(requestId);
        }
    }

    void abandon(std::uint32_t requestId) {
        _pending.erase(requestId);
        _resolved.erase(requestId);
        if (requestId != 0) {
            _retired.insert(requestId);
        }
    }

    bool isRetired(std::uint32_t requestId) const {
        return requestId != 0 && _retired.find(requestId) != _retired.end();
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

    std::optional<RpcPayload> tryTakeAnyResolved() {
        auto it = _resolved.find(0);
        if (it == _resolved.end()) {
            return std::nullopt;
        }
        auto payload = std::move(it->second);
        _resolved.erase(it);
        return payload;
    }

    void reset() {
        _pending.clear();
        _resolved.clear();
        _retired.clear();
    }

private:
    std::map<std::uint32_t, RpcPayload> _pending;
    std::map<std::uint32_t, RpcPayload> _resolved;
    std::set<std::uint32_t> _retired;
};

}  // namespace axtp
