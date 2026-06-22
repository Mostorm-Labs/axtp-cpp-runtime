#pragma once

#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "profiles/media/json_helpers.hpp"
#include "profiles/media/media_types.hpp"
#include "stream/stream_registry.hpp"

namespace axtp::mediahost {

class MediaStreamRegistry : private axtp::stream::IStreamSink {
public:
    explicit MediaStreamRegistry(MediaHostOptions options = {}, LogFn log = {})
        : _options(std::move(options)),
          _log(std::move(log)),
          _streams(axtp::stream::StreamCoreOptions{_options.dumpDir, this}, _log) {}

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
        return axtp::stream::StreamRegistry::shouldLogChunkCount(count);
    }

    bool hasOpenStream(MediaKind kind, std::string_view source) const {
        return _streams.hasOpenStream(kindName(kind), source);
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
            return error(ErrorCode::NotSupported, kindName(kind) + " disabled");
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
        if (auto info = _streams.findStream(streamId)) {
            alreadyClosed = false;
            if (kindFromStreamInfo(*info) != kind) {
                return error(ErrorCode::StreamNotFound, "stream kind mismatch");
            }
            _streams.close(streamId);
        }

        nlohmann::json body = {
            {"streamId", streamId},
            {"state", "closed"},
            {"alreadyClosed", alreadyClosed},
        };
        logLine(kindName(kind) + " closeStream streamId=" + axtp::stream::toHexU32(streamId) +
                (alreadyClosed ? " alreadyClosed=true" : " closed"));
        return {ErrorCode::Success, std::move(body)};
    }

    void handleStream(const StreamPayload& stream) {
        _streams.handleStream(stream);
    }

    MediaStreamStats stats() const {
        MediaStreamStats output;
        {
            std::lock_guard<std::mutex> lock(_statsMutex);
            output = _mediaStats;
        }
        const auto streamStats = _streams.stats();
        output.unknownChunks = streamStats.unknownChunks;
        output.seqGaps = streamStats.seqGaps;
        output.duplicateSeq = streamStats.duplicateSeq;
        return output;
    }

    std::size_t activeStreamCount() const {
        return _streams.activeStreamCount();
    }

    std::vector<ActiveMediaStream> activeStreamsSnapshot() const {
        std::vector<ActiveMediaStream> snapshot;
        const auto active = _streams.activeStreamsSnapshot();
        snapshot.reserve(active.size());
        for (const auto& stream : active) {
            snapshot.push_back(
                ActiveMediaStream{kindFromName(stream.kind), stream.streamId, stream.source});
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
    static MediaKind kindFromName(std::string_view kind) {
        return kind == "audio" ? MediaKind::Audio : MediaKind::Video;
    }

    static MediaKind kindFromStreamInfo(const axtp::stream::StreamInfo& info) {
        return kindFromName(info.kind);
    }

    static MediaStreamInfo toMediaInfo(const axtp::stream::StreamInfo& info) {
        MediaStreamInfo media;
        media.kind = kindFromStreamInfo(info);
        media.streamId = info.streamId;
        media.source = info.source;
        media.codec = info.payloadFormat;
        media.streamProfile = info.streamProfile;
        media.cursorUnit = info.cursorUnit;
        media.metadata = info.metadata;
        media.width = jsonU32Or(info.metadata, "width", jsonU32Or(info.metadata, "codedWidth", 0));
        media.height =
            jsonU32Or(info.metadata, "height", jsonU32Or(info.metadata, "codedHeight", 0));
        media.sampleRate = jsonU32Or(info.metadata, "sampleRate", 0);
        media.channels = jsonU32Or(info.metadata, "channels", 0);
        return media;
    }

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
        nlohmann::json body = {
            {"streamId", streamId},
            {"state", "streaming"},
            {"source", source},
            {"peerRole", std::move(peerRole)},
            {"codec", codec},
            {"streamProfile", streamProfile},
            {"cursorUnit", cursorUnit},
        };
        if (!syncGroupId.empty()) {
            body["syncGroupId"] = syncGroupId;
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

        axtp::stream::StreamInfo info;
        info.kind = kindName(kind);
        info.streamId = streamId;
        info.source = body.value("source", std::string());
        info.payloadFormat = body.value("codec", std::string());
        info.streamProfile = body.value("streamProfile", std::string());
        info.cursorUnit = body.value("cursorUnit", std::string());
        info.metadata = body;

        const auto status = _streams.registerStream(
            info,
            axtp::stream::StreamRegisterOptions{
                kindName(kind),
                kind == MediaKind::Video ? ".h264" : ".aac",
                true,
            });
        if (status != ErrorCode::Success) {
            return error(status, "stream open rejected by axtp_stream");
        }

        logLine(kindName(kind) + " " + logAction +
                " streamId=" + axtp::stream::toHexU32(streamId) +
                " source=" + body.value("source", std::string()) +
                " codec=" + body.value("codec", std::string()) +
                " streamProfile=" + body.value("streamProfile", std::string()) +
                " cursorUnit=" + body.value("cursorUnit", std::string()) +
                " result=" + body.dump());
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

    void onStreamOpened(const axtp::stream::StreamInfo& info) override {
        if (_options.streamSink != nullptr) {
            _options.streamSink->onStreamOpened(toMediaInfo(info));
        }
    }

    void onStreamChunk(const axtp::stream::StreamInfo& info,
                       const StreamPayload& stream) override {
        const auto kind = kindFromStreamInfo(info);
        {
            std::lock_guard<std::mutex> lock(_statsMutex);
            if (kind == MediaKind::Video) {
                ++_mediaStats.videoChunks;
                _mediaStats.videoBytes += stream.data.size();
            } else {
                ++_mediaStats.audioChunks;
                _mediaStats.audioBytes += stream.data.size();
            }
        }
        if (_options.streamSink != nullptr) {
            _options.streamSink->onStreamChunk(kind, stream);
        }
    }

    void onStreamClosed(const axtp::stream::StreamInfo& info) override {
        if (_options.streamSink != nullptr) {
            _options.streamSink->onStreamClosed(kindFromStreamInfo(info), info.streamId);
        }
    }

    void logLine(const std::string& line) const {
        if (_log) {
            _log(line);
        }
    }

    MediaHostOptions _options;
    LogFn _log;
    axtp::stream::StreamRegistry _streams;
    mutable std::mutex _statsMutex;
    MediaStreamStats _mediaStats;
    std::uint32_t _nextVideoStreamId = 0x00001001;
    std::uint32_t _nextAudioStreamId = 0x00002001;
};

} // namespace axtp::mediahost
