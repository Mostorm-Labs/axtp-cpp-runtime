#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axtp.hpp"

namespace axtp::mediahost {

using LogFn = std::function<void(std::string_view)>;

enum class MediaKind {
    Video,
    Audio,
};

enum class OpenMode {
    ReceiverPull,
    ProducerOpen,
    Both,
};

inline const char* openModeName(OpenMode mode) {
    switch (mode) {
    case OpenMode::ReceiverPull:
        return "receiver-pull";
    case OpenMode::ProducerOpen:
        return "producer-open";
    case OpenMode::Both:
        return "both";
    }
    return "receiver-pull";
}

inline bool receiverPullEnabled(OpenMode mode) {
    return mode == OpenMode::ReceiverPull || mode == OpenMode::Both;
}

inline bool producerOpenEnabled(OpenMode mode) {
    return mode == OpenMode::ProducerOpen || mode == OpenMode::Both;
}

struct MediaHostOptions {
    bool acceptVideo = true;
    bool acceptAudio = true;
    bool logBody = false;
    OpenMode openMode = OpenMode::ReceiverPull;
    std::filesystem::path dumpDir;
    std::string source = "wireless_cast";
    std::string audioFormat = "adts";
};

struct MediaStreamStats {
    std::uint64_t videoChunks = 0;
    std::uint64_t audioChunks = 0;
    std::uint64_t videoBytes = 0;
    std::uint64_t audioBytes = 0;
    std::uint64_t unknownChunks = 0;
    std::uint64_t seqGaps = 0;
    std::uint64_t duplicateSeq = 0;
};

struct OpenStreamResult {
    ErrorCode status = ErrorCode::Success;
    nlohmann::json body = nlohmann::json::object();
};

inline std::string toHexU32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

inline Bytes bytesFromString(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

inline const char* errorName(ErrorCode code) {
    const auto* descriptor = RegistryLookup::errorByCode(code);
    return descriptor != nullptr ? descriptor->name : "UNKNOWN_ERROR";
}

inline std::string jsonStringOr(const nlohmann::json& object,
                                const char* name,
                                std::string fallback) {
    const auto it = object.find(name);
    if (it != object.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return fallback;
}

inline std::uint32_t jsonU32Or(const nlohmann::json& object,
                               const char* name,
                               std::uint32_t fallback) {
    const auto it = object.find(name);
    if (it == object.end()) {
        return fallback;
    }
    try {
        if (it->is_number_unsigned()) {
            const auto value = it->get<std::uint64_t>();
            return value <= std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(value)
                       : fallback;
        }
        if (it->is_number_integer()) {
            const auto value = it->get<std::int64_t>();
            return value >= 0 &&
                           static_cast<std::uint64_t>(value) <=
                               std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(value)
                       : fallback;
        }
    } catch (const std::exception&) {
    }
    return fallback;
}

class MediaStreamRegistry {
public:
    explicit MediaStreamRegistry(MediaHostOptions options = {}, LogFn log = {})
        : _options(std::move(options))
        , _log(std::move(log)) {}

    OpenMode openMode() const {
        return _options.openMode;
    }

    bool receiverPullEnabled() const {
        return ::axtp::mediahost::receiverPullEnabled(_options.openMode);
    }

    bool producerOpenEnabled() const {
        return ::axtp::mediahost::producerOpenEnabled(_options.openMode);
    }

    bool mediaEnabled(MediaKind kind) const {
        return kind == MediaKind::Video ? _options.acceptVideo : _options.acceptAudio;
    }

    std::string sourceFor(MediaKind kind) const {
        if (_options.source.empty() || _options.source == "wireless_cast") {
            return kind == MediaKind::Video ? "wireless_cast_video" : "wireless_cast_audio";
        }
        return _options.source;
    }

    static std::string kindName(MediaKind kind) {
        return kind == MediaKind::Video ? "video" : "audio";
    }

    bool hasOpenStream(MediaKind kind, std::string_view source) const {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& entry : _streams) {
            const auto& context = entry.second;
            if (context.kind == kind && context.source == source) {
                return true;
            }
        }
        return false;
    }

    OpenStreamResult acceptProducerOpen(MediaKind kind, std::string_view paramsText) {
        if (!producerOpenEnabled()) {
            return error(ErrorCode::RpcParamInvalid,
                         kindName(kind) +
                             ".openStream came from device, but MediaHost is in receiver-pull "
                             "mode; use --open-mode producer-open or both");
        }
        if (kind == MediaKind::Video && !_options.acceptVideo) {
            return error(ErrorCode::NotSupported, "video disabled");
        }
        if (kind == MediaKind::Audio && !_options.acceptAudio) {
            return error(ErrorCode::NotSupported, "audio disabled");
        }

        nlohmann::json params = nlohmann::json::object();
        if (!paramsText.empty()) {
            try {
                params = nlohmann::json::parse(paramsText);
            } catch (const std::exception&) {
                return error(ErrorCode::RpcParamInvalid, "openStream params are not JSON");
            }
        }
        if (!params.is_object()) {
            return error(ErrorCode::RpcParamInvalid, "openStream params must be an object");
        }

        const auto defaultSource = sourceFor(kind);
        const auto source = jsonStringOr(params, "source", defaultSource);
        const auto peerRole = jsonStringOr(params, "peerRole", "receiver");
        const auto syncGroupId = jsonStringOr(params, "syncGroupId", "");
        const auto castSessionId = jsonStringOr(params, "castSessionId", "");
        const auto requestedMaxDataSize = jsonU32Or(params, "maxDataSize", 0);

        if (kind == MediaKind::Video) {
            const auto codec = jsonStringOr(params, "codec", "h264");
            if (codec != "h264") {
                return error(ErrorCode::MediaCodecUnsupported, "video codec must be h264");
            }
            return openAccepted(kind,
                                allocateStreamId(kind),
                                source,
                                peerRole,
                                "h264",
                                "media.video",
                                "timestampUs",
                                syncGroupId,
                                castSessionId,
                                requestedMaxDataSize,
                                nlohmann::json{{"codecFormat", "annexb"},
                                               {"parameterSetsInKeyFrame", true}},
                                "device open accepted");
        }

        const auto codec = jsonStringOr(params, "codec", "aac");
        if (codec != "aac") {
            return error(ErrorCode::MediaCodecUnsupported, "audio codec must be aac");
        }
        const auto transportFormat =
            jsonStringOr(params, "transportFormat", _options.audioFormat.empty()
                                                           ? std::string("adts")
                                                           : _options.audioFormat);
        if (transportFormat != "adts") {
            return error(ErrorCode::MediaCodecUnsupported,
                         "axtp-mediahost MVP accepts AAC ADTS only");
        }
        const auto sampleRate = jsonU32Or(params, "sampleRate", 48000);
        const auto channels = jsonU32Or(params, "channels", 2);
        return openAccepted(kind,
                            allocateStreamId(kind),
                            source,
                            peerRole,
                            "aac",
                            "media.audio",
                            "timestampUs",
                            syncGroupId,
                            castSessionId,
                            requestedMaxDataSize,
                            nlohmann::json{{"transportFormat", transportFormat},
                                           {"sampleRate", sampleRate},
                                           {"channels", channels}},
                            "device open accepted");
    }

    OpenStreamResult registerPulledOpen(MediaKind kind, std::string_view responseText) {
        if (!mediaEnabled(kind)) {
            return error(kind == MediaKind::Video ? ErrorCode::NotSupported
                                                  : ErrorCode::NotSupported,
                         kindName(kind) + " disabled");
        }

        nlohmann::json result = nlohmann::json::object();
        if (!responseText.empty()) {
            try {
                result = nlohmann::json::parse(responseText);
            } catch (const std::exception&) {
                return error(ErrorCode::RpcPayloadInvalid, "openStream response is not JSON");
            }
        }
        if (!result.is_object()) {
            return error(ErrorCode::RpcPayloadInvalid, "openStream response must be an object");
        }

        const auto streamId = jsonU32Or(result, "streamId", 0);
        if (streamId == 0) {
            return error(ErrorCode::RpcPayloadInvalid, "openStream response missing streamId");
        }

        const auto source = jsonStringOr(result, "source", sourceFor(kind));
        const auto peerRole = jsonStringOr(result, "peerRole", "transmitter");
        const auto syncGroupId = jsonStringOr(result, "syncGroupId", "");
        const auto castSessionId = jsonStringOr(result, "castSessionId", "");
        const auto maxDataSize = jsonU32Or(result, "maxDataSize", 0);

        if (kind == MediaKind::Video) {
            const auto codec = jsonStringOr(result, "codec", "h264");
            if (codec != "h264") {
                return error(ErrorCode::MediaCodecUnsupported,
                             "pulled video codec must be h264");
            }
            return openAccepted(kind,
                                streamId,
                                source,
                                peerRole,
                                codec,
                                jsonStringOr(result, "streamProfile", "media.video"),
                                jsonStringOr(result, "cursorUnit", "timestampUs"),
                                syncGroupId,
                                castSessionId,
                                maxDataSize,
                                result,
                                "receiver pull opened");
        }

        const auto codec = jsonStringOr(result, "codec", "aac");
        if (codec != "aac") {
            return error(ErrorCode::MediaCodecUnsupported, "pulled audio codec must be aac");
        }
        const auto transportFormat = jsonStringOr(result, "transportFormat", "adts");
        if (transportFormat != "adts") {
            return error(ErrorCode::MediaCodecUnsupported,
                         "pulled audio must use AAC ADTS in this MVP");
        }
        return openAccepted(kind,
                            streamId,
                            source,
                            peerRole,
                            codec,
                            jsonStringOr(result, "streamProfile", "media.audio"),
                            jsonStringOr(result, "cursorUnit", "timestampUs"),
                            syncGroupId,
                            castSessionId,
                            maxDataSize,
                            result,
                            "receiver pull opened");
    }

    OpenStreamResult close(MediaKind kind, std::string_view paramsText) {
        nlohmann::json params = nlohmann::json::object();
        if (!paramsText.empty()) {
            try {
                params = nlohmann::json::parse(paramsText);
            } catch (const std::exception&) {
                return error(ErrorCode::RpcParamInvalid, "closeStream params are not JSON");
            }
        }
        const auto streamId = jsonU32Or(params, "streamId", 0);
        if (streamId == 0) {
            return error(ErrorCode::RpcParamMissing, "closeStream requires streamId");
        }

        bool alreadyClosed = true;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _streams.find(streamId);
            if (it != _streams.end()) {
                alreadyClosed = false;
                if (it->second.kind != kind) {
                    return error(ErrorCode::StreamNotFound, "stream kind mismatch");
                }
                if (it->second.dump.is_open()) {
                    it->second.dump.close();
                }
                _streams.erase(it);
            }
        }

        nlohmann::json body = {
            {"streamId", streamId},
            {"state", "closed"},
            {"alreadyClosed", alreadyClosed},
        };
        logLine(kindName(kind) + " closeStream streamId=" + toHexU32(streamId) +
                (alreadyClosed ? " alreadyClosed=true" : " closed"));
        return {ErrorCode::Success, std::move(body)};
    }

    void handleStream(const StreamPayload& stream) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _streams.find(stream.streamId);
        if (it == _streams.end()) {
            ++_stats.unknownChunks;
            logLine("STREAM unknown streamId=" + toHexU32(stream.streamId) +
                    " seq=" + std::to_string(stream.seqId) +
                    " bytes=" + std::to_string(stream.data.size()));
            return;
        }

        auto& context = it->second;
        if (context.hasSeq) {
            if (stream.seqId == context.expectedSeq - 1U) {
                ++_stats.duplicateSeq;
            } else if (stream.seqId != context.expectedSeq) {
                ++_stats.seqGaps;
                logLine(kindName(context.kind) + " seq gap streamId=" +
                        toHexU32(stream.streamId) + " expected=" +
                        std::to_string(context.expectedSeq) + " got=" +
                        std::to_string(stream.seqId));
            }
        }
        context.hasSeq = true;
        context.expectedSeq = stream.seqId + 1U;
        context.lastCursor = stream.cursor;
        context.chunks += 1;
        context.bytes += stream.data.size();

        if (context.kind == MediaKind::Video) {
            ++_stats.videoChunks;
            _stats.videoBytes += stream.data.size();
        } else {
            ++_stats.audioChunks;
            _stats.audioBytes += stream.data.size();
        }

        if (context.dump.is_open() && !stream.data.empty()) {
            context.dump.write(reinterpret_cast<const char*>(stream.data.data()),
                               static_cast<std::streamsize>(stream.data.size()));
        }

        if (context.chunks == 1 || (context.chunks % 100) == 0) {
            logLine(kindName(context.kind) + " STREAM streamId=" + toHexU32(stream.streamId) +
                    " chunks=" + std::to_string(context.chunks) +
                    " bytes=" + std::to_string(context.bytes) +
                    " cursor=" + std::to_string(stream.cursor));
        }
    }

    MediaStreamStats stats() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _stats;
    }

    std::size_t activeStreamCount() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _streams.size();
    }

private:
    struct StreamContext {
        MediaKind kind = MediaKind::Video;
        std::uint32_t streamId = 0;
        std::string source;
        std::string codec;
        std::string streamProfile;
        std::string cursorUnit;
        std::string syncGroupId;
        std::uint32_t expectedSeq = 0;
        bool hasSeq = false;
        std::uint64_t lastCursor = 0;
        std::uint64_t chunks = 0;
        std::uint64_t bytes = 0;
        std::ofstream dump;
    };

    OpenStreamResult openAccepted(MediaKind kind,
                                  std::uint32_t streamId,
                                  std::string source,
                                  std::string peerRole,
                                  std::string codec,
                                  std::string streamProfile,
                                  std::string cursorUnit,
                                  std::string syncGroupId,
                                  std::string castSessionId,
                                  std::uint32_t requestedMaxDataSize,
                                  nlohmann::json extra,
                                  std::string logAction) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_streams.find(streamId) != _streams.end()) {
                return error(ErrorCode::StreamAlreadyOpen,
                             "streamId already open: " + toHexU32(streamId));
            }
            for (const auto& entry : _streams) {
                const auto& existing = entry.second;
                if (existing.kind == kind && existing.source == source) {
                    return error(ErrorCode::StreamAlreadyOpen,
                                 kindName(kind) + " source already open: " + source);
                }
            }
        }

        StreamContext context;
        context.kind = kind;
        context.streamId = streamId;
        context.source = std::move(source);
        context.codec = std::move(codec);
        context.streamProfile = std::move(streamProfile);
        context.cursorUnit = std::move(cursorUnit);
        context.syncGroupId = std::move(syncGroupId);

        if (!_options.dumpDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(_options.dumpDir, ec);
            const auto path = _options.dumpDir /
                              (kindName(kind) + "-" + toHexU32(context.streamId) +
                               (kind == MediaKind::Video ? ".h264" : ".aac"));
            context.dump.open(path, std::ios::binary);
            if (context.dump.is_open()) {
                logLine(kindName(kind) + " dump=" + path.string());
            } else {
                logLine(kindName(kind) + " dump open failed: " + path.string());
            }
        }

        nlohmann::json body = {
            {"streamId", context.streamId},
            {"state", "streaming"},
            {"source", context.source},
            {"peerRole", std::move(peerRole)},
            {"codec", context.codec},
            {"streamProfile", context.streamProfile},
            {"cursorUnit", context.cursorUnit},
        };
        if (!context.syncGroupId.empty()) {
            body["syncGroupId"] = context.syncGroupId;
        }
        if (!castSessionId.empty()) {
            body["castSessionId"] = castSessionId;
        }
        if (requestedMaxDataSize != 0) {
            body["maxDataSize"] = requestedMaxDataSize;
        }
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            body[it.key()] = it.value();
        }

        const auto logName = kindName(kind);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _streams.emplace(context.streamId, std::move(context));
        }
        logLine(logName + " " + logAction + " streamId=" + toHexU32(streamId) +
                " source=" + body.value("source", std::string()) +
                " codec=" + body.value("codec", std::string()) +
                " streamProfile=" + body.value("streamProfile", std::string()) +
                " cursorUnit=" + body.value("cursorUnit", std::string()) +
                " result=" + body.dump());
        return {ErrorCode::Success, std::move(body)};
    }

    OpenStreamResult error(ErrorCode status, std::string detail) const {
        if (!detail.empty()) {
            logLine("open/close rejected status=" + std::to_string(static_cast<std::uint16_t>(status)) +
                    " detail=" + detail);
        }
        return {status, nlohmann::json::object()};
    }

    std::uint32_t allocateStreamId(MediaKind kind) {
        if (kind == MediaKind::Video) {
            return _nextVideoStreamId++;
        }
        return _nextAudioStreamId++;
    }

    void logLine(const std::string& line) const {
        if (_log) {
            _log(line);
        }
    }

    MediaHostOptions _options;
    LogFn _log;
    mutable std::mutex _mutex;
    std::map<std::uint32_t, StreamContext> _streams;
    MediaStreamStats _stats;
    std::uint32_t _nextVideoStreamId = 0x00001001;
    std::uint32_t _nextAudioStreamId = 0x00002001;
};

class MediaPullCoordinator {
public:
    MediaPullCoordinator(MediaStreamRegistry& registry,
                         std::string sid,
                         std::chrono::milliseconds requestTimeout,
                         LogFn log = {})
        : _registry(registry)
        , _sid(std::move(sid))
        , _requestTimeout(requestTimeout)
        , _log(std::move(log)) {}

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
        eventLog << "source event " << MediaStreamRegistry::kindName(*kind)
                 << " source=" << source
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
            logLine("pull state cleared for " + MediaStreamRegistry::kindName(*kind) +
                    " source=" + source + " state=" + state +
                    (erased != 0 ? "" : " (nothing pending)"));
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
            if (it->second.stage == PullStage::Open &&
                !_registry.hasOpenStream(*kind, source)) {
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
        logLine("pull queued: " + MediaStreamRegistry::kindName(*kind) + " source=" +
                _pulls.at(key).source);
        return true;
    }

    template <typename Endpoint>
    void poll(Endpoint& endpoint) {
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
                        " source=" + pull.source +
                        " requestId=" + std::to_string(pull.requestId));
                eraseKeys.push_back(entry.first);
            }
        }
        for (const auto& key : eraseKeys) {
            _pulls.erase(key);
        }

        for (auto& entry : _pulls) {
            auto& pull = entry.second;
            if (pull.stage != PullStage::Queued) {
                continue;
            }
            if (_sid.empty()) {
                if (!pull.waitingSidLogged) {
                    pull.waitingSidLogged = true;
                    logLine("pull waiting for session sid: " +
                            MediaStreamRegistry::kindName(pull.kind) + " source=" + pull.source);
                }
                continue;
            }
            if (_registry.hasOpenStream(pull.kind, pull.source)) {
                pull.stage = PullStage::Open;
                logLine("pull marked open: stream already registered for " +
                        MediaStreamRegistry::kindName(pull.kind) + " source=" + pull.source);
                continue;
            }
            sendOpen(endpoint, pull);
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
        bool waitingSidLogged = false;
        std::chrono::steady_clock::time_point sentAt{};
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
        return static_cast<std::uint16_t>(kind == MediaKind::Video
                                             ? MethodId::VideoOpenStream
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
                              {"sampleRate", 48000},
                              {"channels", 2},
                              {"streamProfile", "media.audio"},
                              {"cursorUnit", "timestampUs"}};
    }

    template <typename Endpoint>
    void sendOpen(Endpoint& endpoint, PullRequest& pull) {
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
        pull.sentAt = std::chrono::steady_clock::now();
        pull.stage = PullStage::Pending;
        logLine("pull send: requestId=" + std::to_string(pull.requestId) +
                " method=" + methodNameFor(pull.kind) +
                " source=" + pull.source +
                " payload=" + paramsText);
        endpoint.sendRpcRequest(std::move(request));
    }

    void handleResponse(PullRequest& pull,
                        const RpcPayload& response,
                        std::vector<std::string>* eraseKeys) {
        const auto bodyText = std::string(response.body.begin(), response.body.end());
        if (response.statusCode != ErrorCode::Success) {
            logLine("pull failed: requestId=" + std::to_string(pull.requestId) +
                    " method=" + methodNameFor(pull.kind) +
                    " source=" + pull.source +
                    " status=" + errorName(response.statusCode) +
                    (bodyText.empty() ? "" : " body=" + bodyText));
            eraseKeys->push_back(keyFor(pull.kind, pull.source));
            return;
        }

        const auto registered = _registry.registerPulledOpen(pull.kind, bodyText);
        if (registered.status != ErrorCode::Success) {
            logLine("pull response rejected locally: requestId=" +
                    std::to_string(pull.requestId) +
                    " method=" + methodNameFor(pull.kind) +
                    " source=" + pull.source +
                    " status=" + errorName(registered.status));
            eraseKeys->push_back(keyFor(pull.kind, pull.source));
            return;
        }
        pull.stage = PullStage::Open;
        logLine("pull success: requestId=" + std::to_string(pull.requestId) +
                " method=" + methodNameFor(pull.kind) +
                " source=" + pull.source +
                (bodyText.empty() ? "" : " result=" + bodyText));
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

inline void installMediaHostHandlers(BasicBroker<>& broker, MediaStreamRegistry& registry) {
    auto jsonHandler = [&registry](MediaKind kind, bool open, const RpcRequestView& request) {
        const std::string params(request.body.begin(), request.body.end());
        const auto result =
            open ? registry.acceptProducerOpen(kind, params) : registry.close(kind, params);
        RpcResponseData response;
        response.encoding = RpcEncoding::Json;
        response.overrideEncoding = true;
        response.statusCode = result.status;
        response.overrideStatus = true;
        if (result.status == ErrorCode::Success) {
            const auto text = result.body.dump();
            response.body.assign(text.begin(), text.end());
        }
        return response;
    };

    broker.registerRawMethod(
        static_cast<std::uint16_t>(MethodId::VideoOpenStream),
        [jsonHandler](const RpcContext&, const RpcRequestView& request) {
            return jsonHandler(MediaKind::Video, true, request);
        });
    broker.registerRawMethod(
        static_cast<std::uint16_t>(MethodId::AudioOpenStream),
        [jsonHandler](const RpcContext&, const RpcRequestView& request) {
            return jsonHandler(MediaKind::Audio, true, request);
        });
    broker.registerRawMethod(
        static_cast<std::uint16_t>(MethodId::VideoCloseStream),
        [jsonHandler](const RpcContext&, const RpcRequestView& request) {
            return jsonHandler(MediaKind::Video, false, request);
        });
    broker.registerRawMethod(
        static_cast<std::uint16_t>(MethodId::AudioCloseStream),
        [jsonHandler](const RpcContext&, const RpcRequestView& request) {
            return jsonHandler(MediaKind::Audio, false, request);
        });
    broker.registerStreamHandler([&registry](const BrokerContext&, const StreamPayload& stream) {
        registry.handleStream(stream);
    });
}

}  // namespace axtp::mediahost
