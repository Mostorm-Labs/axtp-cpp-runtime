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

#include <nlohmann/json.hpp>

#include "axtp.hpp"

namespace axtp::mediahost {

using LogFn = std::function<void(std::string_view)>;

enum class MediaKind {
    Video,
    Audio,
};

struct MediaHostOptions {
    bool acceptVideo = true;
    bool acceptAudio = true;
    bool logBody = false;
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

    OpenStreamResult open(MediaKind kind, std::string_view paramsText) {
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
                                source,
                                peerRole,
                                "h264",
                                "media.video",
                                "timestampUs",
                                syncGroupId,
                                castSessionId,
                                requestedMaxDataSize,
                                nlohmann::json{{"codecFormat", "annexb"},
                                               {"parameterSetsInKeyFrame", true}});
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
                                           {"channels", channels}});
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
                                  std::string source,
                                  std::string peerRole,
                                  std::string codec,
                                  std::string streamProfile,
                                  std::string cursorUnit,
                                  std::string syncGroupId,
                                  std::string castSessionId,
                                  std::uint32_t requestedMaxDataSize,
                                  nlohmann::json extra) {
        StreamContext context;
        context.kind = kind;
        context.streamId = allocateStreamId(kind);
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

        const auto streamId = context.streamId;
        const auto logName = kindName(kind);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _streams.emplace(streamId, std::move(context));
        }
        logLine(logName + " openStream accepted streamId=" + toHexU32(streamId) +
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

    std::string sourceFor(MediaKind kind) const {
        if (_options.source.empty() || _options.source == "wireless_cast") {
            return kind == MediaKind::Video ? "wireless_cast_video" : "wireless_cast_audio";
        }
        return _options.source;
    }

    static std::string kindName(MediaKind kind) {
        return kind == MediaKind::Video ? "video" : "audio";
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

inline void installMediaHostHandlers(BasicBroker<>& broker, MediaStreamRegistry& registry) {
    auto jsonHandler = [&registry](MediaKind kind, bool open, const RpcRequestView& request) {
        const std::string params(request.body.begin(), request.body.end());
        const auto result = open ? registry.open(kind, params) : registry.close(kind, params);
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
