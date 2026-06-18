class WasapiRenderer {
public:
    explicit WasapiRenderer(LogFn log) : _log(std::move(log)) {}

    ~WasapiRenderer() {
        stop();
    }

    bool initialize() {
        if (_audioClient != nullptr) {
            return true;
        }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (failed(hr, _log, "CoCreateInstance(MMDeviceEnumerator)")) {
            return false;
        }
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (failed(hr, _log, "GetDefaultAudioEndpoint(render)")) {
            return false;
        }
        hr = device->Activate(__uuidof(IAudioClient),
                              CLSCTX_ALL,
                              nullptr,
                              reinterpret_cast<void**>(_audioClient.GetAddressOf()));
        if (failed(hr, _log, "IMMDevice::Activate(IAudioClient)")) {
            return false;
        }
        WAVEFORMATEX* mixFormat = nullptr;
        hr = _audioClient->GetMixFormat(&mixFormat);
        if (failed(hr, _log, "IAudioClient::GetMixFormat")) {
            return false;
        }
        _mixFormat.reset(mixFormat);
        _mixPcmFormatValid = pcmFormatFromWaveFormat(*_mixFormat, _mixPcmFormat);
        if (!_mixPcmFormatValid) {
            logLine(_log, "WASAPI mix format is not PCM/float; audio playback disabled");
            return false;
        }
        hr = _audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED, 0, kAudioBufferDuration, 0, _mixFormat.get(), nullptr);
        if (failed(hr, _log, "IAudioClient::Initialize")) {
            return false;
        }
        hr = _audioClient->GetService(IID_PPV_ARGS(&_renderClient));
        if (failed(hr, _log, "IAudioClient::GetService(IAudioRenderClient)")) {
            return false;
        }
        hr = _audioClient->GetBufferSize(&_bufferFrames);
        if (failed(hr, _log, "IAudioClient::GetBufferSize")) {
            return false;
        }
        std::ostringstream out;
        out << "renderer init: WASAPI render endpoint ready sampleRate="
            << _mixFormat->nSamplesPerSec << " channels=" << _mixFormat->nChannels
            << " blockAlign=" << _mixFormat->nBlockAlign
            << " format=" << describePcmFormat(_mixPcmFormat);
        logLine(_log, out.str());
        return true;
    }

    WAVEFORMATEX* mixFormat() const {
        return _mixFormat.get();
    }

    void setMuted(bool muted) {
        const bool previous = _muted.exchange(muted);
        if (previous != muted) {
            logLine(_log, std::string("WASAPI mute state=") + (muted ? "muted" : "unmuted"));
        }
    }

    void stop() {
        if (_audioClient != nullptr && _started) {
            _audioClient->Stop();
        }
        _started = false;
        _renderClient.Reset();
        _audioClient.Reset();
        _mixFormat.reset();
        _mixPcmFormat = {};
        _mixPcmFormatValid = false;
        _conversionLogged = false;
        _formatMismatchLogged = false;
        _bufferFrames = 0;
    }

    void writePcmConverted(const Byte* data, std::size_t size, const PcmFormat& sourceFormat) {
        if (!_mixPcmFormatValid || data == nullptr || size == 0) {
            return;
        }
        if (samePcmFormat(sourceFormat, _mixPcmFormat)) {
            writePcm(data, size);
            return;
        }
        const auto inputBytesPerSample = bytesPerSample(sourceFormat);
        const auto outputBytesPerSample = bytesPerSample(_mixPcmFormat);
        const bool supportedInput =
            (sourceFormat.floatingPoint && sourceFormat.bitsPerSample == 32) ||
            (!sourceFormat.floatingPoint &&
             (sourceFormat.bitsPerSample == 16 || sourceFormat.bitsPerSample == 32));
        const bool supportedOutput =
            (_mixPcmFormat.floatingPoint && _mixPcmFormat.bitsPerSample == 32) ||
            (!_mixPcmFormat.floatingPoint &&
             (_mixPcmFormat.bitsPerSample == 16 || _mixPcmFormat.bitsPerSample == 32));
        if (sourceFormat.sampleRate != _mixPcmFormat.sampleRate || sourceFormat.channels == 0 ||
            _mixPcmFormat.channels == 0 || sourceFormat.blockAlign == 0 ||
            _mixPcmFormat.blockAlign == 0 || inputBytesPerSample == 0 ||
            outputBytesPerSample == 0 || !supportedInput || !supportedOutput) {
            if (!_formatMismatchLogged) {
                _formatMismatchLogged = true;
                logLine(_log,
                        "WASAPI PCM conversion unsupported source=" +
                            describePcmFormat(sourceFormat) +
                            " mix=" + describePcmFormat(_mixPcmFormat));
            }
            return;
        }
        if (!_conversionLogged) {
            _conversionLogged = true;
            logLine(_log,
                    "WASAPI PCM conversion enabled source=" +
                        describePcmFormat(sourceFormat) + " mix=" +
                        describePcmFormat(_mixPcmFormat));
        }
        const auto frames = size / sourceFormat.blockAlign;
        if (frames == 0) {
            return;
        }
        std::vector<Byte> converted(frames * _mixPcmFormat.blockAlign);
        for (std::size_t frameIndex = 0; frameIndex < frames; ++frameIndex) {
            const Byte* inputFrame =
                data + frameIndex * static_cast<std::size_t>(sourceFormat.blockAlign);
            Byte* outputFrame =
                converted.data() + frameIndex * static_cast<std::size_t>(_mixPcmFormat.blockAlign);
            for (std::uint32_t outChannel = 0; outChannel < _mixPcmFormat.channels; ++outChannel) {
                float sample = 0.0f;
                if (_mixPcmFormat.channels == 1 && sourceFormat.channels > 1) {
                    for (std::uint32_t inChannel = 0; inChannel < sourceFormat.channels;
                         ++inChannel) {
                        sample += readPcmSampleAsFloat(inputFrame, sourceFormat, inChannel);
                    }
                    sample /= static_cast<float>(sourceFormat.channels);
                } else {
                    const auto inChannel =
                        sourceFormat.channels == 1
                            ? 0U
                            : std::min<std::uint32_t>(outChannel, sourceFormat.channels - 1U);
                    sample = readPcmSampleAsFloat(inputFrame, sourceFormat, inChannel);
                }
                writeFloatToPcmSample(outputFrame, _mixPcmFormat, outChannel, sample);
            }
        }
        writePcm(converted.data(), converted.size());
    }

    void writePcm(const Byte* data, std::size_t size) {
        if (_audioClient == nullptr || _renderClient == nullptr || _mixFormat == nullptr ||
            size == 0) {
            return;
        }
        const auto blockAlign = _mixFormat->nBlockAlign;
        if (blockAlign == 0) {
            return;
        }
        UINT32 frames = static_cast<UINT32>(size / blockAlign);
        if (frames == 0) {
            return;
        }
        UINT32 padding = 0;
        HRESULT hr = _audioClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            logLine(_log, "WASAPI GetCurrentPadding failed: " + hresultToString(hr));
            return;
        }
        const UINT32 available = _bufferFrames > padding ? _bufferFrames - padding : 0;
        if (available == 0) {
            logLine(_log, "WASAPI render buffer full; dropping PCM chunk");
            return;
        }
        if (frames > available) {
            frames = available;
        }
        BYTE* output = nullptr;
        hr = _renderClient->GetBuffer(frames, &output);
        if (FAILED(hr)) {
            logLine(_log, "WASAPI GetBuffer failed: " + hresultToString(hr));
            return;
        }
        const auto bytesToWrite = static_cast<std::size_t>(frames) * blockAlign;
        if (_muted.load()) {
            std::memset(output, 0, bytesToWrite);
        } else {
            std::memcpy(output, data, bytesToWrite);
        }
        hr = _renderClient->ReleaseBuffer(frames, 0);
        if (FAILED(hr)) {
            logLine(_log, "WASAPI ReleaseBuffer failed: " + hresultToString(hr));
            return;
        }
        if (!_started) {
            hr = _audioClient->Start();
            if (FAILED(hr)) {
                logLine(_log, "WASAPI start failed: " + hresultToString(hr));
                return;
            }
            _started = true;
            logLine(_log, "WASAPI start");
        }
    }

private:
    struct WaveFormatDeleter {
        void operator()(WAVEFORMATEX* value) const {
            if (value != nullptr) {
                CoTaskMemFree(value);
            }
        }
    };

    LogFn _log;
    std::atomic_bool _muted{false};
    bool _started = false;
    UINT32 _bufferFrames = 0;
    std::unique_ptr<WAVEFORMATEX, WaveFormatDeleter> _mixFormat;
    PcmFormat _mixPcmFormat;
    bool _mixPcmFormatValid = false;
    bool _conversionLogged = false;
    bool _formatMismatchLogged = false;
    ComPtr<IAudioClient> _audioClient;
    ComPtr<IAudioRenderClient> _renderClient;
};

class AacMftDecoder {
public:
    explicit AacMftDecoder(LogFn log) : _log(std::move(log)) {}

    void reset() {
        _decoder.Reset();
        _streamingStarted = false;
        _sampleRate = 0;
        _channels = 0;
        _samplingIndex = 0;
        _profile = 0;
        _feedFullAdtsFrames = false;
        _inputModeName = "none";
        _outputFormat = {};
    }

    bool initialize(const AdtsConfig& config, const WAVEFORMATEX& outputFormat) {
        if (_decoder != nullptr && _sampleRate == config.sampleRate &&
            _channels == config.channels && _samplingIndex == config.samplingIndex &&
            _profile == config.profile) {
            return true;
        }
        reset();
        _sampleRate = config.sampleRate;
        _channels = config.channels;
        _samplingIndex = config.samplingIndex;
        _profile = config.profile;

        static const std::array<InputAttempt, 4> kAttempts = {{
            {"aac/adts", MFAudioFormat_AAC, 1, UserDataKind::HeAac, true},
            {"adts-subtype", MFAudioFormat_ADTS, 1, UserDataKind::None, true},
            {"aac/raw", MFAudioFormat_AAC, 0, UserDataKind::HeAac, false},
            {"raw-aac1", MEDIASUBTYPE_RAW_AAC1, 0, UserDataKind::AscOnly, false},
        }};
        for (const auto& attempt : kAttempts) {
            if (tryInitializeAttempt(config, outputFormat, attempt)) {
                return true;
            }
            _decoder.Reset();
            _streamingStarted = false;
        }
        logLine(_log,
                "AAC decoder input config rejected by all modes sampleRate=" +
                    std::to_string(_sampleRate) + " channels=" + std::to_string(_channels) +
                    " profile=" + std::to_string(_profile) +
                    " samplingIndex=" + std::to_string(_samplingIndex));
        reset();
        return false;
    }

    bool expectsFullAdtsFrames() const {
        return _feedFullAdtsFrames;
    }

    template <typename Callback>
    void
    processFrame(const Byte* frame, std::size_t frameSize, std::uint64_t cursor, Callback&& onPcm) {
        if (_decoder == nullptr || frame == nullptr || frameSize == 0) {
            return;
        }
        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateSample(&sample);
        if (failed(hr, _log, "MFCreateSample(AAC input)")) {
            return;
        }
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(frameSize), &buffer);
        if (failed(hr, _log, "MFCreateMemoryBuffer(AAC input)")) {
            return;
        }
        Byte* data = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        hr = buffer->Lock(&data, &maxLength, &currentLength);
        if (failed(hr, _log, "AAC input buffer Lock")) {
            return;
        }
        std::memcpy(data, frame, frameSize);
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(frameSize));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(cursor * 10ULL));

        if (!_streamingStarted) {
            _decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            _decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            _streamingStarted = true;
        }
        hr = _decoder->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            drainOutput(onPcm);
            hr = _decoder->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            logLine(_log, "AAC decoder ProcessInput failed: " + hresultToString(hr));
            return;
        }
        drainOutput(std::forward<Callback>(onPcm));
    }

private:
    enum class UserDataKind {
        None,
        AscOnly,
        HeAac,
    };

    struct InputAttempt {
        const char* name;
        GUID subtype;
        std::uint32_t payloadType;
        UserDataKind userDataKind;
        bool feedFullAdtsFrames;
    };

    static void appendLe16(Bytes& out, std::uint16_t value) {
        out.push_back(static_cast<Byte>(value & 0xFFU));
        out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
    }

    static void appendLe32(Bytes& out, std::uint32_t value) {
        out.push_back(static_cast<Byte>(value & 0xFFU));
        out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
        out.push_back(static_cast<Byte>((value >> 16U) & 0xFFU));
        out.push_back(static_cast<Byte>((value >> 24U) & 0xFFU));
    }

    static Bytes audioSpecificConfig(const AdtsConfig& config) {
        const std::uint32_t objectType = std::max<std::uint32_t>(1, config.profile + 1U);
        const std::uint32_t samplingIndex = config.samplingIndex & 0x0FU;
        const std::uint32_t channelConfig = config.channelConfig & 0x0FU;
        return Bytes{
            static_cast<Byte>((objectType << 3U) | (samplingIndex >> 1U)),
            static_cast<Byte>(((samplingIndex & 0x01U) << 7U) | (channelConfig << 3U)),
        };
    }

    static Bytes heAacUserData(const AdtsConfig& config, std::uint32_t payloadType) {
        Bytes out;
        out.reserve(12 + 2);
        appendLe16(out, static_cast<std::uint16_t>(payloadType));
        appendLe16(out, 0x00FEU);
        appendLe16(out, 0);
        appendLe16(out, 0);
        appendLe32(out, 0);
        const auto asc = audioSpecificConfig(config);
        out.insert(out.end(), asc.begin(), asc.end());
        return out;
    }

    bool tryInitializeAttempt(const AdtsConfig& config,
                              const WAVEFORMATEX& outputFormat,
                              const InputAttempt& attempt) {
        HRESULT hr = CoCreateInstance(
            CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_decoder));
        if (failed(hr, _log, "CoCreateInstance(CMSAACDecMFT)")) {
            return false;
        }

        ComPtr<IMFMediaType> inputType;
        hr = MFCreateMediaType(&inputType);
        if (failed(hr, _log, "MFCreateMediaType(AAC input)")) {
            return false;
        }
        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inputType->SetGUID(MF_MT_SUBTYPE, attempt.subtype);
        inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, _sampleRate);
        inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, _channels);
        inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 0);
        inputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, attempt.payloadType);
        inputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xFE);

        Bytes userData;
        if (attempt.userDataKind == UserDataKind::HeAac) {
            userData = heAacUserData(config, attempt.payloadType);
        } else if (attempt.userDataKind == UserDataKind::AscOnly) {
            userData = audioSpecificConfig(config);
        }
        if (!userData.empty()) {
            inputType->SetBlob(MF_MT_USER_DATA,
                               userData.data(),
                               static_cast<UINT32>(userData.size()));
        }

        std::ostringstream inputLog;
        inputLog << "AAC decoder trying input mode=" << attempt.name
                 << " sampleRate=" << _sampleRate << " channels=" << _channels
                 << " payloadType=" << attempt.payloadType
                 << " userDataBytes=" << userData.size()
                 << " feed=" << (attempt.feedFullAdtsFrames ? "full-adts" : "raw-aac");
        logLine(_log, inputLog.str());

        hr = _decoder->SetInputType(0, inputType.Get(), 0);
        if (FAILED(hr)) {
            logLine(_log,
                    "AAC decoder SetInputType rejected mode=" + std::string(attempt.name) +
                        ": " + hresultToString(hr));
            return false;
        }
        _inputModeName = attempt.name;
        _feedFullAdtsFrames = attempt.feedFullAdtsFrames;
        if (!selectOutputType(outputFormat)) {
            return false;
        }
        std::ostringstream out;
        out << "renderer init: AAC decoder ready mode=" << _inputModeName
            << " input=" << _sampleRate << "Hz/" << _channels
            << "ch output=" << describePcmFormat(_outputFormat)
            << " feed=" << (_feedFullAdtsFrames ? "full-adts" : "raw-aac");
        logLine(_log, out.str());
        return true;
    }

    bool selectOutputType(const WAVEFORMATEX& outputFormat) {
        std::vector<PcmFormat> candidates;
        PcmFormat mixFormat;
        if (pcmFormatFromWaveFormat(outputFormat, mixFormat)) {
            candidates.push_back(mixFormat);
            PcmFormat decodedLikeMix = mixFormat;
            decodedLikeMix.sampleRate = _sampleRate;
            decodedLikeMix.channels = _channels;
            decodedLikeMix.blockAlign =
                decodedLikeMix.channels * (decodedLikeMix.bitsPerSample / 8U);
            if (decodedLikeMix.blockAlign != 0) {
                candidates.push_back(decodedLikeMix);
            }
        }
        candidates.push_back(PcmFormat{_sampleRate, _channels, 16, _channels * 2U, false});
        candidates.push_back(PcmFormat{_sampleRate, _channels, 32, _channels * 4U, true});

        std::vector<PcmFormat> uniqueCandidates;
        for (const auto& candidate : candidates) {
            if (candidate.sampleRate == 0 || candidate.channels == 0 ||
                candidate.bitsPerSample == 0 || candidate.blockAlign == 0) {
                continue;
            }
            const auto duplicate =
                std::find_if(uniqueCandidates.begin(),
                             uniqueCandidates.end(),
                             [&candidate](const PcmFormat& existing) {
                                 return samePcmFormat(candidate, existing);
                             });
            if (duplicate == uniqueCandidates.end()) {
                uniqueCandidates.push_back(candidate);
            }
        }

        for (const auto& candidate : uniqueCandidates) {
            ComPtr<IMFMediaType> outputType;
            HRESULT hr = MFCreateMediaType(&outputType);
            if (failed(hr, _log, "MFCreateMediaType(AAC output)")) {
                return false;
            }
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            outputType->SetGUID(MF_MT_SUBTYPE,
                                candidate.floatingPoint ? MFAudioFormat_Float
                                                        : MFAudioFormat_PCM);
            outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, candidate.sampleRate);
            outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, candidate.channels);
            outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, candidate.bitsPerSample);
            outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, candidate.blockAlign);
            outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                  candidate.sampleRate * candidate.blockAlign);
            hr = _decoder->SetOutputType(0, outputType.Get(), 0);
            if (SUCCEEDED(hr)) {
                _outputFormat = candidate;
                return true;
            }
            logLine(_log,
                    "AAC decoder SetOutputType rejected output=" +
                        describePcmFormat(candidate) + ": " + hresultToString(hr));
        }
        return false;
    }

    template <typename Callback> void drainOutput(Callback&& onPcm) {
        MFT_OUTPUT_STREAM_INFO streamInfo = {};
        _decoder->GetOutputStreamInfo(0, &streamInfo);
        while (true) {
            ComPtr<IMFSample> outputSample;
            HRESULT hr = MFCreateSample(&outputSample);
            if (failed(hr, _log, "MFCreateSample(AAC output)")) {
                return;
            }
            ComPtr<IMFMediaBuffer> outputBuffer;
            hr = MFCreateMemoryBuffer(streamInfo.cbSize == 0 ? 256 * 1024 : streamInfo.cbSize,
                                      &outputBuffer);
            if (failed(hr, _log, "MFCreateMemoryBuffer(AAC output)")) {
                return;
            }
            outputSample->AddBuffer(outputBuffer.Get());

            MFT_OUTPUT_DATA_BUFFER output = {};
            output.dwStreamID = 0;
            output.pSample = outputSample.Get();
            DWORD status = 0;
            hr = _decoder->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents != nullptr) {
                output.pEvents->Release();
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                logLine(_log, "AAC decoder stream change; continuing with negotiated output");
                continue;
            }
            if (FAILED(hr)) {
                logLine(_log, "AAC decoder ProcessOutput failed: " + hresultToString(hr));
                return;
            }

            ComPtr<IMFMediaBuffer> contiguous;
            hr = outputSample->ConvertToContiguousBuffer(&contiguous);
            if (failed(hr, _log, "AAC output ConvertToContiguousBuffer")) {
                return;
            }
            Byte* data = nullptr;
            DWORD maxLength = 0;
            DWORD currentLength = 0;
            hr = contiguous->Lock(&data, &maxLength, &currentLength);
            if (failed(hr, _log, "AAC output buffer Lock")) {
                return;
            }
            onPcm(data, currentLength, _outputFormat);
            contiguous->Unlock();
        }
    }

    LogFn _log;
    ComPtr<IMFTransform> _decoder;
    bool _streamingStarted = false;
    std::uint32_t _sampleRate = 0;
    std::uint32_t _channels = 0;
    std::uint32_t _samplingIndex = 0;
    std::uint32_t _profile = 0;
    bool _feedFullAdtsFrames = false;
    std::string _inputModeName = "none";
    PcmFormat _outputFormat;
};
