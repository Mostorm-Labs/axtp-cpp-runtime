#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "media/protocol/media_stream_registry.hpp"

namespace axtp::mediahost {

class MediaPullCoordinator {
public:
    MediaPullCoordinator(MediaStreamRegistry& registry,
                         std::string sid,
                         std::chrono::milliseconds requestTimeout,
                         LogFn log = {})
        : _registry(registry), _sid(std::move(sid)), _requestTimeout(requestTimeout),
          _log(std::move(log)) {}

    void setSid(std::string sid) {
        _sid = std::move(sid);
    }

    bool handleEvent(const RpcPayload& event) {
        const auto kind = kindFromSourceEvent(event);
        if (!kind.has_value()) {
            return false;
        }
        if (!_registry.receiverPullEnabled()) {
            return true;
        }

        nlohmann::json body = nlohmann::json::object();
        if (!event.body.empty()) {
            try {
                body = nlohmann::json::parse(std::string(event.body.begin(), event.body.end()));
            } catch (const std::exception&) {
                logLine("source event ignored: body is not JSON");
                return true;
            }
        }
        if (!body.is_object()) {
            logLine("source event ignored: body must be an object");
            return true;
        }

        auto source = jsonStringOr(body, "source", "");
        if (source.empty()) {
            source = _registry.sourceFor(*kind);
            logLine("source event has no source; fallback source=" + source);
        }
        const auto state = jsonStringOr(body, "state", "");
        const auto reason = jsonStringOr(body, "reason", "");
        const auto activeStreamId = jsonU32Or(body, "activeStreamId", 0);

        std::ostringstream eventLog;
        eventLog << "source event " << MediaStreamRegistry::kindName(*kind) << " source=" << source
                 << " state=" << (state.empty() ? "<missing>" : state)
                 << " reason=" << (reason.empty() ? "<none>" : reason);
        if (activeStreamId != 0) {
            eventLog << " activeStreamId=" << toHexU32(activeStreamId);
        }
        logLine(eventLog.str());

        if (!_registry.mediaEnabled(*kind)) {
            logLine("pull skipped: " + MediaStreamRegistry::kindName(*kind) + " disabled");
            return true;
        }
        if (state == "stopped" || state == "unavailable" || state == "failed") {
            const auto erased = _pulls.erase(keyFor(*kind, source));
            logLine("pull state cleared for " + MediaStreamRegistry::kindName(*kind) + " source=" +
                    source + " state=" + state + (erased != 0 ? "" : " (nothing pending)"));
            return true;
        }
        if (state != "available" && state != "receiving") {
            logLine("pull not started: source state is not available/receiving");
            return true;
        }
        if (_registry.hasOpenStream(*kind, source)) {
            logLine("pull skipped: " + MediaStreamRegistry::kindName(*kind) +
                    " source already open");
            return true;
        }

        const auto key = keyFor(*kind, source);
        auto it = _pulls.find(key);
        if (it != _pulls.end()) {
            if (it->second.stage == PullStage::Open && !_registry.hasOpenStream(*kind, source)) {
                _pulls.erase(it);
            } else {
                logLine("pull skipped: " + MediaStreamRegistry::kindName(*kind) +
                        " source=" + source + " already " + pullStageName(it->second.stage));
                return true;
            }
        }

        if (_registry.hasOpenStream(*kind, source)) {
            logLine("pull skipped: " + MediaStreamRegistry::kindName(*kind) +
                    " source already open");
            return true;
        }

        PullRequest pull;
        pull.kind = *kind;
        pull.source = std::move(source);
        pull.stage = PullStage::Queued;
        _pulls.emplace(key, std::move(pull));
        logLine("pull queued: " + MediaStreamRegistry::kindName(*kind) +
                " source=" + _pulls.at(key).source);
        return true;
    }

    template <typename Endpoint> void poll(Endpoint& endpoint) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> eraseKeys;
        for (auto& entry : _pulls) {
            auto& pull = entry.second;
            if (pull.stage != PullStage::Pending) {
                continue;
            }
            if (auto response = endpoint.tryTakeRpcResponse(pull.requestId)) {
                handleResponse(pull, *response, &eraseKeys);
                continue;
            }
            if (now - pull.sentAt >= _requestTimeout) {
                logLine("pull timeout: " + MediaStreamRegistry::kindName(pull.kind) +
                        " source=" + pull.source + " requestId=" + std::to_string(pull.requestId));
                scheduleRetry(pull, ErrorCode::RpcResponseTimeout, "");
            }
        }
        for (const auto& key : eraseKeys) {
            _pulls.erase(key);
        }

        for (const auto kind : {MediaKind::Video, MediaKind::Audio}) {
            for (auto& entry : _pulls) {
                auto& pull = entry.second;
                if (pull.stage != PullStage::Queued || pull.kind != kind) {
                    continue;
                }
                if (pull.retryAfter.time_since_epoch().count() != 0 && now < pull.retryAfter) {
                    continue;
                }
                if (pull.kind == MediaKind::Audio &&
                    _registry.mediaEnabled(MediaKind::Video) &&
                    (!hasOpenVideoStream() || hasUnfinishedVideoPull())) {
                    if (!pull.waitingVideoLogged) {
                        pull.waitingVideoLogged = true;
                        logLine("pull waiting: audio source=" + pull.source +
                                " waits for video open first");
                    }
                    continue;
                }
                if (_sid.empty()) {
                    if (!pull.waitingSidLogged) {
                        pull.waitingSidLogged = true;
                        logLine("pull waiting for session sid: " +
                                MediaStreamRegistry::kindName(pull.kind) +
                                " source=" + pull.source);
                    }
                    continue;
                }
                if (_registry.hasOpenStream(pull.kind, pull.source)) {
                    pull.stage = PullStage::Open;
                    logLine("pull marked open: stream already registered for " +
                            MediaStreamRegistry::kindName(pull.kind) +
                            " source=" + pull.source);
                    continue;
                }
                sendOpen(endpoint, pull);
            }
        }
    }

    std::size_t pendingCount() const {
        std::size_t count = 0;
        for (const auto& entry : _pulls) {
            if (entry.second.stage != PullStage::Open) {
                ++count;
            }
        }
        return count;
    }

    void clearAll(std::string_view reason) {
        const auto count = _pulls.size();
        _pulls.clear();
        logLine("pull state cleared: reason=" + std::string(reason) +
                " entries=" + std::to_string(count));
    }

private:
    enum class PullStage {
        Queued,
        Pending,
        Open,
    };

    struct PullRequest {
        MediaKind kind = MediaKind::Video;
        std::string source;
        PullStage stage = PullStage::Queued;
        std::uint32_t requestId = 0;
        std::uint32_t attempts = 0;
        bool waitingSidLogged = false;
        bool waitingVideoLogged = false;
        std::chrono::steady_clock::time_point sentAt{};
        std::chrono::steady_clock::time_point retryAfter{};
    };

    static std::optional<MediaKind> kindFromSourceEvent(const RpcPayload& event) {
        if (event.methodOrEventId ==
            static_cast<std::uint16_t>(EventId::VideoStreamSourceStateChanged)) {
            return MediaKind::Video;
        }
        if (event.methodOrEventId ==
            static_cast<std::uint16_t>(EventId::AudioStreamSourceStateChanged)) {
            return MediaKind::Audio;
        }
        return std::nullopt;
    }

    static std::string keyFor(MediaKind kind, std::string_view source) {
        return MediaStreamRegistry::kindName(kind) + std::string(":") + std::string(source);
    }

    static const char* pullStageName(PullStage stage) {
        switch (stage) {
        case PullStage::Queued:
            return "queued";
        case PullStage::Pending:
            return "pending";
        case PullStage::Open:
            return "open";
        }
        return "queued";
    }

    static std::uint16_t methodIdFor(MediaKind kind) {
        return static_cast<std::uint16_t>(kind == MediaKind::Video ? MethodId::VideoOpenStream
                                                                   : MethodId::AudioOpenStream);
    }

    static const char* methodNameFor(MediaKind kind) {
        return kind == MediaKind::Video ? "video.openStream" : "audio.openStream";
    }

    nlohmann::json openParamsFor(const PullRequest& pull) const {
        if (pull.kind == MediaKind::Video) {
            return nlohmann::json{{"source", pull.source},
                                  {"peerRole", "transmitter"},
                                  {"codec", "h264"},
                                  {"streamProfile", "media.video"},
                                  {"cursorUnit", "timestampUs"}};
        }
        return nlohmann::json{{"source", pull.source},
                              {"peerRole", "transmitter"},
                              {"codec", "aac"},
                              {"transportFormat", "adts"},
                              {"sampleRate", _registry.audioSampleRate()},
                              {"channels", _registry.audioChannels()},
                              {"streamProfile", "media.audio"},
                              {"cursorUnit", "timestampUs"}};
    }

    template <typename Endpoint> void sendOpen(Endpoint& endpoint, PullRequest& pull) {
        auto params = openParamsFor(pull);
        const auto paramsText = params.dump();

        RpcPayload request;
        request.encoding = RpcEncoding::Json;
        request.op = RpcOp::Request;
        request.requestId = _nextRequestId++;
        if (_nextRequestId == 0) {
            _nextRequestId = 1;
        }
        request.methodOrEventId = methodIdFor(pull.kind);
        request.statusCode = ErrorCode::Success;
        request.bodyEncoding = RpcBodyEncoding::None;
        request.meta.sourceProtocol = SourceProtocol::JsonRpc;
        request.meta.jsonSid = _sid;
        request.meta.jsonMethodOrEventName = methodNameFor(pull.kind);
        request.body.assign(paramsText.begin(), paramsText.end());

        pull.requestId = request.requestId;
        ++pull.attempts;
        pull.sentAt = std::chrono::steady_clock::now();
        pull.stage = PullStage::Pending;
        logLine("pull send: requestId=" + std::to_string(pull.requestId) + " method=" +
                methodNameFor(pull.kind) + " source=" + pull.source +
                " attempt=" + std::to_string(pull.attempts) + " payload=" + paramsText);
        endpoint.sendRpcRequest(std::move(request));
    }

    void handleResponse(PullRequest& pull,
                        const RpcPayload& response,
                        std::vector<std::string>* eraseKeys) {
        const auto bodyText = std::string(response.body.begin(), response.body.end());
        if (response.statusCode != ErrorCode::Success) {
            logLine("pull failed: requestId=" + std::to_string(pull.requestId) +
                    " method=" + methodNameFor(pull.kind) + " source=" + pull.source + " status=" +
                    errorName(response.statusCode) + (bodyText.empty() ? "" : " body=" + bodyText));
            if (isRetryableOpenStatus(response.statusCode)) {
                scheduleRetry(pull, response.statusCode, bodyText);
                return;
            }
            eraseKeys->push_back(keyFor(pull.kind, pull.source));
            return;
        }

        const auto registered = _registry.registerPulledOpen(pull.kind, bodyText);
        if (registered.status != ErrorCode::Success) {
            logLine("pull response rejected locally: requestId=" + std::to_string(pull.requestId) +
                    " method=" + methodNameFor(pull.kind) + " source=" + pull.source +
                    " status=" + errorName(registered.status));
            eraseKeys->push_back(keyFor(pull.kind, pull.source));
            return;
        }
        pull.stage = PullStage::Open;
        pull.retryAfter = {};
        logLine("pull success: requestId=" + std::to_string(pull.requestId) +
                " method=" + methodNameFor(pull.kind) + " source=" + pull.source +
                (bodyText.empty() ? "" : " result=" + bodyText));
    }

    static bool isRetryableOpenStatus(ErrorCode status) {
        return status == ErrorCode::Unavailable ||
               status == ErrorCode::MediaSourceUnavailable ||
               status == ErrorCode::Busy ||
               status == ErrorCode::DeviceResourceBusy ||
               status == ErrorCode::Timeout ||
               status == ErrorCode::RpcResponseTimeout;
    }

    static std::chrono::milliseconds retryDelayForAttempt(std::uint32_t attempts) {
        if (attempts <= 1) {
            return std::chrono::milliseconds(500);
        }
        if (attempts == 2) {
            return std::chrono::milliseconds(1000);
        }
        if (attempts == 3) {
            return std::chrono::milliseconds(2000);
        }
        return std::chrono::milliseconds(3000);
    }

    void scheduleRetry(PullRequest& pull, ErrorCode status, std::string_view bodyText) {
        const auto delay = retryDelayForAttempt(pull.attempts);
        pull.stage = PullStage::Queued;
        pull.requestId = 0;
        pull.retryAfter = std::chrono::steady_clock::now() + delay;
        pull.waitingSidLogged = false;
        if (pull.kind == MediaKind::Video) {
            pull.waitingVideoLogged = false;
        }
        logLine("pull retry scheduled: method=" + std::string(methodNameFor(pull.kind)) +
                " source=" + pull.source +
                " status=" + errorName(status) +
                " attempt=" + std::to_string(pull.attempts) +
                " retryDelayMs=" + std::to_string(delay.count()) +
                (bodyText.empty() ? "" : " body=" + std::string(bodyText)));
    }

    bool hasUnfinishedVideoPull() const {
        for (const auto& entry : _pulls) {
            const auto& pull = entry.second;
            if (pull.kind == MediaKind::Video && pull.stage != PullStage::Open) {
                return true;
            }
        }
        return false;
    }

    bool hasOpenVideoStream() const {
        for (const auto& stream : _registry.activeStreamsSnapshot()) {
            if (stream.kind == MediaKind::Video) {
                return true;
            }
        }
        return false;
    }

    void logLine(const std::string& line) const {
        if (_log) {
            _log(line);
        }
    }

    MediaStreamRegistry& _registry;
    std::string _sid;
    std::chrono::milliseconds _requestTimeout;
    LogFn _log;
    std::map<std::string, PullRequest> _pulls;
    std::uint32_t _nextRequestId = 1;
};

} // namespace axtp::mediahost
