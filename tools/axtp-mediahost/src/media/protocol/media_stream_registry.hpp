#pragma once

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "media/model/media_types.hpp"
#include "media/protocol/json_helpers.hpp"

namespace axtp::mediahost {

class MediaStreamRegistry {
public:
    explicit MediaStreamRegistry(MediaHostOptions options = {}, LogFn log = {})
        : _options(std::move(options)), _log(std::move(log)) {}

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

    std::uint32_t audioSampleRate() const {
        return _options.audioSampleRate == 0 ? 48000 : _options.audioSampleRate;
    }

    std::uint32_t audioChannels() const {
        return _options.audioChannels == 0 ? 1 : _options.audioChannels;
    }

    static std::string kindName(MediaKind kind) {
        return kind == MediaKind::Video ? "video" : "audio";
    }

    static bool shouldLogChunkCount(std::uint64_t count) {
        return count <= 50 || (count % 100) == 0;
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
            return openAccepted(
                kind,
                allocateStreamId(kind),
                source,
                peerRole,
                "h264",
                "media.video",
                "timestampUs",
                syncGroupId,
                castSessionId,
                requestedMaxDataSize,
                nlohmann::json{{"codecFormat", "annexb"}, {"parameterSetsInKeyFrame", true}},
                "device open accepted");
        }

        const auto codec = jsonStringOr(params, "codec", "aac");
        if (codec != "aac") {
            return error(ErrorCode::MediaCodecUnsupported, "audio codec must be aac");
        }
        const auto transportFormat =
            jsonStringOr(params,
                         "transportFormat",
                         _options.audioFormat.empty() ? std::string("adts") : _options.audioFormat);
        if (transportFormat != "adts") {
            return error(ErrorCode::MediaCodecUnsupported,
                         "axtp-mediahost MVP accepts AAC ADTS only");
        }
        const auto sampleRate = jsonU32Or(params, "sampleRate", audioSampleRate());
        const auto channels = jsonU32Or(params, "channels", audioChannels());
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
                return error(ErrorCode::MediaCodecUnsupported, "pulled video codec must be h264");
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
        if (!result.contains("sampleRate")) {
            result["sampleRate"] = audioSampleRate();
        }
        if (!result.contains("channels")) {
            result["channels"] = audioChannels();
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
        if (!alreadyClosed && _options.streamSink != nullptr) {
            _options.streamSink->onStreamClosed(kind, streamId);
        }
        return {ErrorCode::Success, std::move(body)};
    }

    void handleStream(const StreamPayload& stream) {
        MediaKind kind = MediaKind::Video;
        bool knownStream = false;
        {
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
            kind = context.kind;
            knownStream = true;
            if (context.hasSeq) {
                if (stream.seqId == context.expectedSeq - 1U) {
                    ++_stats.duplicateSeq;
                } else if (stream.seqId != context.expectedSeq) {
                    ++_stats.seqGaps;
                    logLine(kindName(context.kind) +
                            " seq gap streamId=" + toHexU32(stream.streamId) +
                            " expected=" + std::to_string(context.expectedSeq) +
                            " got=" + std::to_string(stream.seqId));
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

            if (shouldLogChunkCount(context.chunks)) {
                logLine(kindName(context.kind) + " STREAM streamId=" + toHexU32(stream.streamId) +
                        " seq=" + std::to_string(stream.seqId) +
                        " chunkBytes=" + std::to_string(stream.data.size()) +
                        " chunks=" + std::to_string(context.chunks) +
                        " totalBytes=" + std::to_string(context.bytes) +
                        " cursor=" + std::to_string(stream.cursor));
            }
        }
        if (knownStream && _options.streamSink != nullptr) {
            _options.streamSink->onStreamChunk(kind, stream);
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

    std::vector<ActiveMediaStream> activeStreamsSnapshot() const {
        std::vector<ActiveMediaStream> snapshot;
        std::lock_guard<std::mutex> lock(_mutex);
        snapshot.reserve(_streams.size());
        for (const auto& entry : _streams) {
            snapshot.push_back(
                ActiveMediaStream{entry.second.kind, entry.second.streamId, entry.second.source});
        }
        return snapshot;
    }

    OpenStreamResult closeLocal(MediaKind kind, std::uint32_t streamId) {
        const nlohmann::json params = {
            {"streamId", streamId},
            {"peerRole", "transmitter"},
        };
        return close(kind, params.dump());
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
            const auto path =
                _options.dumpDir / (kindName(kind) + "-" + toHexU32(context.streamId) +
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

        MediaStreamInfo info;
        info.kind = kind;
        info.streamId = context.streamId;
        info.source = body.value("source", std::string());
        info.codec = body.value("codec", std::string());
        info.streamProfile = body.value("streamProfile", std::string());
        info.cursorUnit = body.value("cursorUnit", std::string());
        info.width = jsonU32Or(body, "width", jsonU32Or(body, "codedWidth", 0));
        info.height = jsonU32Or(body, "height", jsonU32Or(body, "codedHeight", 0));
        info.sampleRate = jsonU32Or(body, "sampleRate", 0);
        info.channels = jsonU32Or(body, "channels", 0);
        info.metadata = body;

        const auto logName = kindName(kind);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _streams.emplace(context.streamId, std::move(context));
        }
        logLine(logName + " " + logAction + " streamId=" + toHexU32(streamId) +
                " source=" + body.value("source", std::string()) +
                " codec=" + body.value("codec", std::string()) +
                " streamProfile=" + body.value("streamProfile", std::string()) + " cursorUnit=" +
                body.value("cursorUnit", std::string()) + " result=" + body.dump());
        if (_options.streamSink != nullptr) {
            _options.streamSink->onStreamOpened(info);
        }
        return {ErrorCode::Success, std::move(body)};
    }

    OpenStreamResult error(ErrorCode status, std::string detail) const {
        if (!detail.empty()) {
            logLine("open/close rejected status=" +
                    std::to_string(static_cast<std::uint16_t>(status)) + " detail=" + detail);
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

} // namespace axtp::mediahost
