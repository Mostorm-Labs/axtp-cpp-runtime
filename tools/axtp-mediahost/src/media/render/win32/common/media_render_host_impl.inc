const char* renderBackendName(RenderBackend backend) {
    switch (backend) {
    case RenderBackend::None:
        return "none";
    case RenderBackend::SelfTest:
        return "self-test";
    case RenderBackend::MfD3d11:
        return "mf-d3d11";
    }
    return "none";
}

RenderBackend parseRenderBackendOrNone(std::string_view text, bool* ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    if (text == "none") {
        return RenderBackend::None;
    }
    if (text == "self-test") {
        return RenderBackend::SelfTest;
    }
    if (text == "mf-d3d11") {
        return RenderBackend::MfD3d11;
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return RenderBackend::None;
}

const char* h264FeedModeName(H264FeedMode mode) {
    switch (mode) {
    case H264FeedMode::StreamChunk:
        return "stream-chunk";
    case H264FeedMode::AnnexBAccessUnit:
        return "annexb-au";
    }
    return "stream-chunk";
}

H264FeedMode parseH264FeedModeOrDefault(std::string_view text, bool* ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    if (text == "stream-chunk") {
        return H264FeedMode::StreamChunk;
    }
    if (text == "annexb-au") {
        return H264FeedMode::AnnexBAccessUnit;
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return H264FeedMode::StreamChunk;
}

MediaRenderHost::MediaRenderHost(LogFn log) : _log(std::move(log)) {}

MediaRenderHost::~MediaRenderHost() {
    stop();
}

bool MediaRenderHost::start(const MediaRenderHostOptions& options) {
    if (_running.load()) {
        return true;
    }
    _options = options;
    _muted.store(options.startMuted);
    if (_options.backend == RenderBackend::None) {
        return true;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        _comInitialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        failed(hr, _log, "CoInitializeEx");
        return false;
    }
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (failed(hr, _log, "MFStartup")) {
        if (_comInitialized) {
            CoUninitialize();
            _comInitialized = false;
        }
        return false;
    }
    _mfStarted = true;

    std::ostringstream initLog;
    initLog << "renderer init: backend=" << renderBackendName(_options.backend)
            << " h264FeedMode=" << h264FeedModeName(_options.h264FeedMode)
            << " overlay=" << (_options.overlayEnabled ? "enabled" : "disabled")
            << " audio=" << (_options.enableAudio ? "enabled" : "disabled")
            << " muted=" << (_muted.load() ? "true" : "false");
    logLine(initLog.str());
    if (_options.enableVideo) {
        _video = std::make_unique<AxtpVideoRenderer>(_log);
        if (!_video->start(_options.backend,
                           _options.h264FeedMode,
                           _options.overlayEnabled,
                           [this]() { return toggleMuted(); },
                           [this]() { return isMuted(); })) {
            stop();
            return false;
        }
    }
    if (_options.enableAudio && _options.backend == RenderBackend::MfD3d11) {
        _audio = std::make_unique<AxtpAudioRenderer>(_log);
        if (!_audio->start(_options.startMuted)) {
            stop();
            return false;
        }
    }
    _running.store(true);
    return true;
}

void MediaRenderHost::stop() {
    if (_video != nullptr) {
        _video->stop();
        _video.reset();
    }
    if (_audio != nullptr) {
        _audio->stop();
        _audio.reset();
    }
    if (_mfStarted) {
        MFShutdown();
        _mfStarted = false;
    }
    if (_comInitialized) {
        CoUninitialize();
        _comInitialized = false;
    }
    _running.store(false);
}

bool MediaRenderHost::running() const {
    if (_options.backend == RenderBackend::None) {
        return false;
    }
    if (_video != nullptr) {
        return _video->running();
    }
    return _running.load();
}

void MediaRenderHost::setStatusText(std::string text) {
    if (_video != nullptr) {
        _video->setStatusText(std::move(text));
    }
}

void MediaRenderHost::setMuted(bool muted) {
    if (!_options.enableAudio) {
        logLine("renderer audio muted request ignored: audio disabled");
        _muted.store(false);
        if (_video != nullptr) {
            _video->setMuted(false);
        }
        return;
    }
    _muted.store(muted);
    _options.startMuted = muted;
    if (_audio != nullptr) {
        _audio->setMuted(muted);
    }
    if (_video != nullptr) {
        _video->setMuted(muted);
    }
}

bool MediaRenderHost::isMuted() const {
    return _muted.load();
}

bool MediaRenderHost::toggleMuted() {
    if (!_options.enableAudio) {
        logLine("renderer audio mute toggle ignored: audio disabled");
        _muted.store(false);
        return false;
    }
    const bool muted = !_muted.load();
    setMuted(muted);
    return muted;
}

bool MediaRenderHost::consumeStopCastingRequested() {
    if (_video == nullptr) {
        return false;
    }
    return _video->consumeStopCastingRequested();
}

void MediaRenderHost::resetVideoDecoder(std::string_view reason) {
    if (_video != nullptr) {
        _video->resetDecoder(reason);
    }
}

void MediaRenderHost::onStreamOpened(const MediaStreamInfo& info) {
    if (info.kind == MediaKind::Video && _video != nullptr) {
        _video->onStreamOpened(info);
    } else if (info.kind == MediaKind::Audio && _audio != nullptr) {
        _audio->onStreamOpened(info);
    }
}

void MediaRenderHost::onStreamChunk(MediaKind kind, const StreamPayload& stream) {
    if (kind == MediaKind::Video && _video != nullptr) {
        _video->onStreamChunk(stream);
    } else if (kind == MediaKind::Audio && _audio != nullptr) {
        _audio->onStreamChunk(stream);
    }
}

void MediaRenderHost::onStreamClosed(MediaKind kind, std::uint32_t streamId) {
    if (kind == MediaKind::Video && _video != nullptr) {
        _video->onStreamClosed(kind, streamId);
    } else if (kind == MediaKind::Audio && _audio != nullptr) {
        _audio->onStreamClosed(kind, streamId);
    }
}

void MediaRenderHost::logLine(std::string_view line) const {
    if (_log) {
        _log(line);
    }
}
