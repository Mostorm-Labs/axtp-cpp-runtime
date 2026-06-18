class AxtpAudioRenderer {
public:
    explicit AxtpAudioRenderer(LogFn log) : _log(std::move(log)), _wasapi(_log), _decoder(_log) {}

    bool start(bool muted) {
        _muted.store(muted);
        _wasapi.setMuted(muted);
        logLine(_log, std::string("renderer init: audio ") + (muted ? "start muted" : "unmuted"));
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(_mutex);
        _parser.reset();
        _decoder.reset();
        _wasapi.stop();
        _streamId = 0;
    }

    void setMuted(bool muted) {
        _muted.store(muted);
        _wasapi.setMuted(muted);
    }

    void onStreamOpened(const MediaStreamInfo& info) {
        if (info.kind != MediaKind::Audio) {
            return;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _streamId = info.streamId;
        _overrideSampleRate = info.sampleRate;
        _overrideChannels = info.channels;
        _parser.reset();
        _decoder.reset();
        _firstFrameLogged = false;
        _decoderDisabled = false;
        _pcmChunkCount = 0;
        _pcmBytes = 0;
        std::ostringstream out;
        out << "renderer init: audio stream opened streamId=" << toHexU32(info.streamId)
            << " codec=" << info.codec << " sampleRate=" << info.sampleRate
            << " channels=" << info.channels;
        logLine(_log, out.str());
    }

    void onStreamChunk(const StreamPayload& stream) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_streamId == 0 || stream.streamId != _streamId) {
            return;
        }
        auto frames = _parser.push(stream.data);
        for (auto frame : frames) {
            const auto headerSampleRate = frame.config.sampleRate;
            const auto headerChannels = frame.config.channels;
            if (_overrideSampleRate != 0 && _overrideSampleRate != frame.config.sampleRate) {
                if (auto index = aacSampleRateIndex(_overrideSampleRate)) {
                    frame.config.sampleRate = _overrideSampleRate;
                    frame.config.samplingIndex = *index;
                }
            }
            if (_overrideChannels != 0 && _overrideChannels != frame.config.channels) {
                frame.config.channels = _overrideChannels;
                frame.config.channelConfig = _overrideChannels;
            }
            if (!_firstFrameLogged) {
                _firstFrameLogged = true;
                logLine(_log,
                        "first audio frame streamId=" + toHexU32(stream.streamId) +
                            " adtsSampleRate=" + std::to_string(headerSampleRate) +
                            " adtsChannels=" + std::to_string(headerChannels) +
                            " decodeSampleRate=" + std::to_string(frame.config.sampleRate) +
                            " decodeChannels=" + std::to_string(frame.config.channels) +
                            " adtsBytes=" + std::to_string(frame.bytes.size()) +
                            " rawBytes=" +
                            std::to_string(frame.bytes.size() > frame.headerSize
                                               ? frame.bytes.size() - frame.headerSize
                                               : 0));
            }
            if (_decoderDisabled) {
                continue;
            }
            if (!_wasapi.initialize()) {
                return;
            }
            auto* mixFormat = _wasapi.mixFormat();
            if (mixFormat == nullptr) {
                return;
            }
            if (!_decoder.initialize(frame.config, *mixFormat)) {
                _decoderDisabled = true;
                logLine(_log,
                        "AAC decoder disabled for current audio stream after init failure");
                return;
            }
            const Byte* inputData = frame.bytes.data();
            std::size_t inputSize = frame.bytes.size();
            if (!_decoder.expectsFullAdtsFrames()) {
                if (frame.bytes.size() <= frame.headerSize) {
                    continue;
                }
                inputData = frame.bytes.data() + frame.headerSize;
                inputSize = frame.bytes.size() - frame.headerSize;
            }
            if (inputSize == 0) {
                continue;
            }
            _decoder.processFrame(
                inputData,
                inputSize,
                stream.cursor,
                [this](const Byte* data, std::size_t size, const PcmFormat& format) {
                    ++_pcmChunkCount;
                    _pcmBytes += size;
                    if (_pcmChunkCount == 1 || (_pcmChunkCount % 100) == 0) {
                        logLine(_log,
                                "audio PCM chunk index=" + std::to_string(_pcmChunkCount) +
                                    " bytes=" + std::to_string(size) +
                                    " format=" + describePcmFormat(format) +
                                    " totalBytes=" + std::to_string(_pcmBytes));
                    }
                    _wasapi.writePcmConverted(data, size, format);
                });
        }
    }

    void onStreamClosed(MediaKind kind, std::uint32_t streamId) {
        if (kind != MediaKind::Audio) {
            return;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        if (_streamId == streamId) {
            logLine(_log, "renderer audio stream closed streamId=" + toHexU32(streamId));
            _parser.reset();
            _decoder.reset();
            _wasapi.stop();
            _streamId = 0;
            _overrideSampleRate = 0;
            _overrideChannels = 0;
            _decoderDisabled = false;
        }
    }

private:
    LogFn _log;
    std::mutex _mutex;
    std::atomic_bool _muted{false};
    std::uint32_t _streamId = 0;
    std::uint32_t _overrideSampleRate = 0;
    std::uint32_t _overrideChannels = 0;
    bool _firstFrameLogged = false;
    bool _decoderDisabled = false;
    std::uint64_t _pcmChunkCount = 0;
    std::uint64_t _pcmBytes = 0;
    AdtsParser _parser;
    WasapiRenderer _wasapi;
    AacMftDecoder _decoder;
};
