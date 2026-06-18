struct AdtsConfig {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t profile = 1;
    std::uint32_t samplingIndex = 3;
    std::uint32_t channelConfig = 2;
};

struct AdtsFrame {
    Bytes bytes;
    AdtsConfig config;
    std::size_t headerSize = 7;
};

std::optional<std::uint32_t> aacSampleRateIndex(std::uint32_t sampleRate) {
    static constexpr std::uint32_t kSampleRates[] = {
        96000,
        88200,
        64000,
        48000,
        44100,
        32000,
        24000,
        22050,
        16000,
        12000,
        11025,
        8000,
        7350,
    };
    for (std::uint32_t i = 0; i < std::size(kSampleRates); ++i) {
        if (kSampleRates[i] == sampleRate) {
            return i;
        }
    }
    return std::nullopt;
}

class AdtsParser {
public:
    void reset() {
        _buffer.clear();
    }

    std::vector<AdtsFrame> push(const Bytes& chunk) {
        _buffer.insert(_buffer.end(), chunk.begin(), chunk.end());
        std::vector<AdtsFrame> frames;
        while (_buffer.size() >= 7) {
            std::size_t sync = 0;
            while (sync + 1 < _buffer.size() &&
                   !(_buffer[sync] == 0xFF && (_buffer[sync + 1] & 0xF0U) == 0xF0U)) {
                ++sync;
            }
            if (sync != 0) {
                _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(sync));
            }
            if (_buffer.size() < 7) {
                break;
            }

            const std::uint32_t samplingIndex = (_buffer[2] & 0x3CU) >> 2U;
            const std::uint32_t profile = (_buffer[2] & 0xC0U) >> 6U;
            const std::uint32_t channelConfig =
                ((_buffer[2] & 0x01U) << 2U) | ((_buffer[3] & 0xC0U) >> 6U);
            const std::uint32_t frameLength = ((_buffer[3] & 0x03U) << 11U) |
                                              (static_cast<std::uint32_t>(_buffer[4]) << 3U) |
                                              ((_buffer[5] & 0xE0U) >> 5U);
            const std::size_t headerSize = (_buffer[1] & 0x01U) != 0 ? 7U : 9U;
            if (frameLength < headerSize) {
                _buffer.erase(_buffer.begin());
                continue;
            }
            if (_buffer.size() < frameLength) {
                break;
            }

            static constexpr std::uint32_t kSampleRates[] = {
                96000,
                88200,
                64000,
                48000,
                44100,
                32000,
                24000,
                22050,
                16000,
                12000,
                11025,
                8000,
                7350,
            };
            AdtsFrame frame;
            frame.config.sampleRate =
                samplingIndex < std::size(kSampleRates) ? kSampleRates[samplingIndex] : 48000;
            frame.config.channels = channelConfig == 0 ? 2 : channelConfig;
            frame.config.profile = profile;
            frame.config.samplingIndex = samplingIndex;
            frame.config.channelConfig = channelConfig == 0 ? frame.config.channels : channelConfig;
            frame.headerSize = headerSize;
            frame.bytes.insert(frame.bytes.end(), _buffer.begin(), _buffer.begin() + frameLength);
            frames.push_back(std::move(frame));
            _buffer.erase(_buffer.begin(),
                          _buffer.begin() + static_cast<std::ptrdiff_t>(frameLength));
        }
        return frames;
    }

private:
    Bytes _buffer;
};

struct PcmFormat {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t bitsPerSample = 0;
    std::uint32_t blockAlign = 0;
    bool floatingPoint = false;
};

bool pcmFormatFromWaveFormat(const WAVEFORMATEX& format, PcmFormat& out) {
    GUID subtype = {};
    bool supported = false;
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        subtype = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        supported = true;
    } else if (format.wFormatTag == WAVE_FORMAT_PCM) {
        subtype = KSDATAFORMAT_SUBTYPE_PCM;
        supported = true;
    } else if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        subtype = extensible.SubFormat;
        supported = IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ||
                    IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_PCM);
    }
    if (!supported) {
        return false;
    }
    out.sampleRate = format.nSamplesPerSec;
    out.channels = format.nChannels;
    out.bitsPerSample = format.wBitsPerSample;
    out.blockAlign = format.nBlockAlign;
    out.floatingPoint = IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    if (out.bitsPerSample == 0 || out.blockAlign == 0 || out.channels == 0 ||
        out.sampleRate == 0) {
        return false;
    }
    return true;
}

std::uint32_t bytesPerSample(const PcmFormat& format) {
    return format.channels == 0 ? 0 : format.blockAlign / format.channels;
}

bool samePcmFormat(const PcmFormat& lhs, const PcmFormat& rhs) {
    return lhs.sampleRate == rhs.sampleRate && lhs.channels == rhs.channels &&
           lhs.bitsPerSample == rhs.bitsPerSample && lhs.blockAlign == rhs.blockAlign &&
           lhs.floatingPoint == rhs.floatingPoint;
}

std::string describePcmFormat(const PcmFormat& format) {
    std::ostringstream out;
    out << format.sampleRate << "Hz/" << format.channels << "ch/"
        << format.bitsPerSample << "bit/" << (format.floatingPoint ? "float" : "pcm");
    return out.str();
}

float readPcmSampleAsFloat(const Byte* frame, const PcmFormat& format, std::uint32_t channel) {
    const auto bps = bytesPerSample(format);
    const Byte* sample = frame + static_cast<std::size_t>(channel) * bps;
    if (format.floatingPoint && format.bitsPerSample == 32 && bps >= sizeof(float)) {
        float value = 0.0f;
        std::memcpy(&value, sample, sizeof(float));
        return std::clamp(value, -1.0f, 1.0f);
    }
    if (!format.floatingPoint && format.bitsPerSample == 16 && bps >= sizeof(std::int16_t)) {
        std::int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return std::clamp(static_cast<float>(value) / 32768.0f, -1.0f, 1.0f);
    }
    if (!format.floatingPoint && format.bitsPerSample == 32 && bps >= sizeof(std::int32_t)) {
        std::int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return std::clamp(static_cast<float>(value) / 2147483648.0f, -1.0f, 1.0f);
    }
    return 0.0f;
}

void writeFloatToPcmSample(Byte* frame,
                           const PcmFormat& format,
                           std::uint32_t channel,
                           float value) {
    const auto bps = bytesPerSample(format);
    Byte* sample = frame + static_cast<std::size_t>(channel) * bps;
    value = std::clamp(value, -1.0f, 1.0f);
    if (format.floatingPoint && format.bitsPerSample == 32 && bps >= sizeof(float)) {
        std::memcpy(sample, &value, sizeof(float));
        return;
    }
    if (!format.floatingPoint && format.bitsPerSample == 16 && bps >= sizeof(std::int16_t)) {
        const auto scaled = static_cast<int>(value * 32767.0f);
        const auto clamped = std::clamp(scaled, -32768, 32767);
        const auto output = static_cast<std::int16_t>(clamped);
        std::memcpy(sample, &output, sizeof(output));
        return;
    }
    if (!format.floatingPoint && format.bitsPerSample == 32 && bps >= sizeof(std::int32_t)) {
        const double scaled = static_cast<double>(value) * 2147483647.0;
        const auto clamped = std::clamp(scaled, -2147483648.0, 2147483647.0);
        const auto output = static_cast<std::int32_t>(clamped);
        std::memcpy(sample, &output, sizeof(output));
    }
}
