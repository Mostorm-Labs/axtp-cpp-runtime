#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axtp_core.hpp"

namespace axtp::mediahost {

using LogFn = std::function<void(std::string_view)>;

enum class MediaKind {
    Video,
    Audio,
};

class IMediaStreamSink;

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
    std::uint32_t audioSampleRate = 48000;
    std::uint32_t audioChannels = 1;
    IMediaStreamSink* streamSink = nullptr;
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

struct MediaStreamInfo {
    MediaKind kind = MediaKind::Video;
    std::uint32_t streamId = 0;
    std::string source;
    std::string codec;
    std::string streamProfile;
    std::string cursorUnit;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    nlohmann::json metadata = nlohmann::json::object();
};

struct ActiveMediaStream {
    MediaKind kind = MediaKind::Video;
    std::uint32_t streamId = 0;
    std::string source;
};

class IMediaStreamSink {
public:
    virtual ~IMediaStreamSink() = default;
    virtual void onStreamOpened(const MediaStreamInfo& info) = 0;
    virtual void onStreamChunk(MediaKind kind, const StreamPayload& stream) = 0;
    virtual void onStreamClosed(MediaKind kind, std::uint32_t streamId) = 0;
};

struct OpenStreamResult {
    ErrorCode status = ErrorCode::Success;
    nlohmann::json body = nlohmann::json::object();
};

} // namespace axtp::mediahost
