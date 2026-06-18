#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "media/model/media_types.hpp"

namespace axtp::mediahost {

enum class RenderBackend {
    None,
    SelfTest,
    MfD3d11,
};

enum class H264FeedMode {
    StreamChunk,
    AnnexBAccessUnit,
};

const char* renderBackendName(RenderBackend backend);
RenderBackend parseRenderBackendOrNone(std::string_view text, bool* ok);
const char* h264FeedModeName(H264FeedMode mode);
H264FeedMode parseH264FeedModeOrDefault(std::string_view text, bool* ok);

struct MediaRenderHostOptions {
    RenderBackend backend = RenderBackend::None;
    H264FeedMode h264FeedMode = H264FeedMode::StreamChunk;
    bool enableVideo = true;
    bool enableAudio = true;
    bool startMuted = false;
    bool overlayEnabled = true;
};

class AxtpVideoRenderer;
class AxtpAudioRenderer;

class MediaRenderHost final : public IMediaStreamSink {
public:
    explicit MediaRenderHost(LogFn log = {});
    ~MediaRenderHost() override;

    MediaRenderHost(const MediaRenderHost&) = delete;
    MediaRenderHost& operator=(const MediaRenderHost&) = delete;

    bool start(const MediaRenderHostOptions& options);
    void stop();
    bool running() const;
    void setStatusText(std::string text);
    void setMuted(bool muted);
    bool isMuted() const;
    bool toggleMuted();
    bool consumeStopCastingRequested();

    void onStreamOpened(const MediaStreamInfo& info) override;
    void onStreamChunk(MediaKind kind, const StreamPayload& stream) override;
    void onStreamClosed(MediaKind kind, std::uint32_t streamId) override;

private:
    void logLine(std::string_view line) const;

    MediaRenderHostOptions _options;
    LogFn _log;
    std::atomic_bool _running{false};
    std::atomic_bool _muted{false};
    bool _comInitialized = false;
    bool _mfStarted = false;
    std::unique_ptr<AxtpVideoRenderer> _video;
    std::unique_ptr<AxtpAudioRenderer> _audio;
};

} // namespace axtp::mediahost
