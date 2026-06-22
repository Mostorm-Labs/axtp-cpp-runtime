#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "stream/stream_registry.hpp"
#include "core/protocol/generated/registry_lookup.h"

namespace axtp::stream {

struct StreamCloseRequest {
    std::uint16_t methodId = 0;
    std::string methodName;
    std::uint32_t streamId = 0;
    std::string source;
};

inline const char* streamErrorName(ErrorCode code) {
    const auto* descriptor = RegistryLookup::errorByCode(code);
    return descriptor != nullptr ? descriptor->name : "UNKNOWN_ERROR";
}

class StreamCloseCoordinator {
public:
    StreamCloseCoordinator(std::string sid,
                           std::chrono::milliseconds requestTimeout,
                           LogFn log = {})
        : _sid(std::move(sid)), _requestTimeout(requestTimeout), _log(std::move(log)) {}

    void setSid(std::string sid) {
        _sid = std::move(sid);
    }

    template <typename Endpoint> void sendClose(Endpoint& endpoint, const StreamCloseRequest& close) {
        if (_sid.empty()) {
            logLine("closeStream skipped: session sid is empty");
            return;
        }
        if (close.streamId == 0 || close.methodId == 0 || close.methodName.empty()) {
            return;
        }

        const auto params = closeParamsFor(close.streamId);
        const auto paramsText = params.dump();

        RpcPayload request;
        request.encoding = RpcEncoding::Json;
        request.op = RpcOp::Request;
        request.requestId = _nextRequestId++;
        if (_nextRequestId == 0) {
            _nextRequestId = 0x40000000U;
        }
        request.methodOrEventId = close.methodId;
        request.statusCode = ErrorCode::Success;
        request.bodyEncoding = RpcBodyEncoding::None;
        request.meta.sourceProtocol = SourceProtocol::JsonRpc;
        request.meta.jsonSid = _sid;
        request.meta.jsonMethodOrEventName = close.methodName;
        request.body.assign(paramsText.begin(), paramsText.end());

        PendingClose pending;
        pending.methodName = close.methodName;
        pending.streamId = close.streamId;
        pending.source = close.source;
        pending.requestId = request.requestId;
        pending.sentAt = std::chrono::steady_clock::now();
        _pending.emplace(pending.requestId, pending);

        logLine("closeStream send: requestId=" + std::to_string(pending.requestId) +
                " method=" + close.methodName +
                " streamId=" + toHexU32(close.streamId) +
                (close.source.empty() ? "" : " source=" + close.source) +
                " payload=" + paramsText);
        endpoint.sendRpcRequest(std::move(request));
    }

    template <typename Endpoint> void poll(Endpoint& endpoint) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::uint32_t> eraseIds;
        for (auto& entry : _pending) {
            auto& pending = entry.second;
            if (auto response = endpoint.tryTakeRpcResponse(pending.requestId)) {
                const auto bodyText = std::string(response->body.begin(), response->body.end());
                logLine("closeStream response: requestId=" + std::to_string(pending.requestId) +
                        " method=" + pending.methodName +
                        " streamId=" + toHexU32(pending.streamId) +
                        " status=" + streamErrorName(response->statusCode) +
                        (bodyText.empty() ? "" : " body=" + bodyText));
                eraseIds.push_back(entry.first);
                continue;
            }
            if (now - pending.sentAt >= _requestTimeout) {
                logLine("closeStream timeout: requestId=" + std::to_string(pending.requestId) +
                        " method=" + pending.methodName +
                        " streamId=" + toHexU32(pending.streamId));
                eraseIds.push_back(entry.first);
            }
        }
        for (const auto requestId : eraseIds) {
            _pending.erase(requestId);
        }
    }

    std::size_t pendingCount() const {
        return _pending.size();
    }

    static nlohmann::json closeParamsFor(std::uint32_t streamId) {
        return nlohmann::json{{"streamId", streamId}, {"peerRole", "transmitter"}};
    }

private:
    struct PendingClose {
        std::string methodName;
        std::uint32_t streamId = 0;
        std::string source;
        std::uint32_t requestId = 0;
        std::chrono::steady_clock::time_point sentAt{};
    };

    void logLine(const std::string& line) const {
        if (_log) {
            _log(line);
        }
    }

    std::string _sid;
    std::chrono::milliseconds _requestTimeout;
    LogFn _log;
    std::map<std::uint32_t, PendingClose> _pending;
    std::uint32_t _nextRequestId = 0x40000000U;
};

} // namespace axtp::stream
