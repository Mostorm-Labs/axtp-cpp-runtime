#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "media/protocol/media_stream_registry.hpp"

namespace axtp::mediahost {

class MediaCloseCoordinator {
public:
    MediaCloseCoordinator(std::string sid,
                          std::chrono::milliseconds requestTimeout,
                          LogFn log = {})
        : _sid(std::move(sid)), _requestTimeout(requestTimeout), _log(std::move(log)) {}

    void setSid(std::string sid) {
        _sid = std::move(sid);
    }

    template <typename Endpoint> void sendClose(Endpoint& endpoint, const ActiveMediaStream& stream) {
        if (_sid.empty()) {
            logLine("closeStream skipped: session sid is empty");
            return;
        }
        if (stream.streamId == 0) {
            return;
        }

        const auto params = closeParamsFor(stream.streamId);
        const auto paramsText = params.dump();

        RpcPayload request;
        request.encoding = RpcEncoding::Json;
        request.op = RpcOp::Request;
        request.requestId = _nextRequestId++;
        if (_nextRequestId == 0) {
            _nextRequestId = 0x40000000U;
        }
        request.methodOrEventId = methodIdFor(stream.kind);
        request.statusCode = ErrorCode::Success;
        request.bodyEncoding = RpcBodyEncoding::None;
        request.meta.sourceProtocol = SourceProtocol::JsonRpc;
        request.meta.jsonSid = _sid;
        request.meta.jsonMethodOrEventName = methodNameFor(stream.kind);
        request.body.assign(paramsText.begin(), paramsText.end());

        PendingClose pending;
        pending.kind = stream.kind;
        pending.streamId = stream.streamId;
        pending.source = stream.source;
        pending.requestId = request.requestId;
        pending.sentAt = std::chrono::steady_clock::now();
        _pending.emplace(pending.requestId, pending);

        logLine("closeStream send: requestId=" + std::to_string(pending.requestId) +
                " method=" + methodNameFor(stream.kind) +
                " streamId=" + toHexU32(stream.streamId) +
                (stream.source.empty() ? "" : " source=" + stream.source) +
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
                        " method=" + methodNameFor(pending.kind) +
                        " streamId=" + toHexU32(pending.streamId) +
                        " status=" + errorName(response->statusCode) +
                        (bodyText.empty() ? "" : " body=" + bodyText));
                eraseIds.push_back(entry.first);
                continue;
            }
            if (now - pending.sentAt >= _requestTimeout) {
                logLine("closeStream timeout: requestId=" + std::to_string(pending.requestId) +
                        " method=" + methodNameFor(pending.kind) +
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
        MediaKind kind = MediaKind::Video;
        std::uint32_t streamId = 0;
        std::string source;
        std::uint32_t requestId = 0;
        std::chrono::steady_clock::time_point sentAt{};
    };

    static std::uint16_t methodIdFor(MediaKind kind) {
        return static_cast<std::uint16_t>(kind == MediaKind::Video ? MethodId::VideoCloseStream
                                                                   : MethodId::AudioCloseStream);
    }

    static const char* methodNameFor(MediaKind kind) {
        return kind == MediaKind::Video ? "video.closeStream" : "audio.closeStream";
    }

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

} // namespace axtp::mediahost
