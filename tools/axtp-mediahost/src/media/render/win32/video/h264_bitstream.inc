struct VideoSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class BitReader {
public:
    explicit BitReader(const std::vector<Byte>& bytes) : _bytes(bytes) {}

    bool readBits(std::uint32_t count, std::uint32_t* output) {
        if (count > 32) {
            return false;
        }
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (_bitOffset >= _bytes.size() * 8U) {
                return false;
            }
            const auto byte = _bytes[_bitOffset / 8U];
            const auto bit = 7U - (_bitOffset % 8U);
            value = (value << 1U) | ((byte >> bit) & 0x01U);
            ++_bitOffset;
        }
        *output = value;
        return true;
    }

    bool readBit(std::uint32_t* output) {
        return readBits(1, output);
    }

    bool readUE(std::uint32_t* output) {
        std::uint32_t zeroes = 0;
        std::uint32_t bit = 0;
        while (readBit(&bit)) {
            if (bit == 1) {
                break;
            }
            ++zeroes;
            if (zeroes > 31) {
                return false;
            }
        }
        if (bit != 1) {
            return false;
        }
        std::uint32_t suffix = 0;
        if (zeroes != 0 && !readBits(zeroes, &suffix)) {
            return false;
        }
        *output = ((1U << zeroes) - 1U) + suffix;
        return true;
    }

    bool readSE(std::int32_t* output) {
        std::uint32_t codeNum = 0;
        if (!readUE(&codeNum)) {
            return false;
        }
        const auto magnitude = static_cast<std::int32_t>((codeNum + 1U) / 2U);
        *output = (codeNum % 2U) == 0 ? -magnitude : magnitude;
        return true;
    }

private:
    const std::vector<Byte>& _bytes;
    std::size_t _bitOffset = 0;
};

std::vector<Byte> rbspFromNal(const Byte* data, std::size_t size) {
    std::vector<Byte> rbsp;
    rbsp.reserve(size);
    int zeroCount = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const Byte value = data[i];
        if (zeroCount == 2 && value == 0x03) {
            zeroCount = 0;
            continue;
        }
        rbsp.push_back(value);
        if (value == 0x00) {
            ++zeroCount;
        } else {
            zeroCount = 0;
        }
    }
    return rbsp;
}

void skipScalingList(BitReader* reader, std::uint32_t sizeOfScalingList) {
    std::int32_t lastScale = 8;
    std::int32_t nextScale = 8;
    for (std::uint32_t i = 0; i < sizeOfScalingList; ++i) {
        if (nextScale != 0) {
            std::int32_t deltaScale = 0;
            if (!reader->readSE(&deltaScale)) {
                return;
            }
            nextScale = (lastScale + deltaScale + 256) % 256;
        }
        lastScale = nextScale == 0 ? lastScale : nextScale;
    }
}

std::optional<VideoSize> parseSps(const Byte* nal, std::size_t size) {
    if (size < 2) {
        return std::nullopt;
    }
    auto rbsp = rbspFromNal(nal + 1, size - 1);
    BitReader reader(rbsp);

    std::uint32_t profileIdc = 0;
    std::uint32_t ignored = 0;
    std::uint32_t chromaFormatIdc = 1;
    if (!reader.readBits(8, &profileIdc) || !reader.readBits(8, &ignored) ||
        !reader.readBits(8, &ignored)) {
        return std::nullopt;
    }
    if (!reader.readUE(&ignored)) {
        return std::nullopt;
    }

    const bool highProfile = profileIdc == 100 || profileIdc == 110 || profileIdc == 122 ||
                             profileIdc == 244 || profileIdc == 44 || profileIdc == 83 ||
                             profileIdc == 86 || profileIdc == 118 || profileIdc == 128 ||
                             profileIdc == 138 || profileIdc == 144;
    if (highProfile) {
        if (!reader.readUE(&chromaFormatIdc)) {
            return std::nullopt;
        }
        if (chromaFormatIdc == 3 && !reader.readBits(1, &ignored)) {
            return std::nullopt;
        }
        if (!reader.readUE(&ignored) || !reader.readUE(&ignored) || !reader.readBits(1, &ignored)) {
            return std::nullopt;
        }
        std::uint32_t scalingMatrixPresent = 0;
        if (!reader.readBit(&scalingMatrixPresent)) {
            return std::nullopt;
        }
        if (scalingMatrixPresent != 0) {
            const std::uint32_t count = chromaFormatIdc != 3 ? 8 : 12;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t present = 0;
                if (!reader.readBit(&present)) {
                    return std::nullopt;
                }
                if (present != 0) {
                    skipScalingList(&reader, i < 6 ? 16 : 64);
                }
            }
        }
    }

    if (!reader.readUE(&ignored)) {
        return std::nullopt;
    }
    std::uint32_t picOrderCntType = 0;
    if (!reader.readUE(&picOrderCntType)) {
        return std::nullopt;
    }
    if (picOrderCntType == 0) {
        if (!reader.readUE(&ignored)) {
            return std::nullopt;
        }
    } else if (picOrderCntType == 1) {
        if (!reader.readBits(1, &ignored)) {
            return std::nullopt;
        }
        std::int32_t ignoredSigned = 0;
        if (!reader.readSE(&ignoredSigned) || !reader.readSE(&ignoredSigned)) {
            return std::nullopt;
        }
        std::uint32_t cycleCount = 0;
        if (!reader.readUE(&cycleCount)) {
            return std::nullopt;
        }
        for (std::uint32_t i = 0; i < cycleCount; ++i) {
            if (!reader.readSE(&ignoredSigned)) {
                return std::nullopt;
            }
        }
    }
    if (!reader.readUE(&ignored) || !reader.readBits(1, &ignored)) {
        return std::nullopt;
    }

    std::uint32_t widthInMbsMinus1 = 0;
    std::uint32_t heightInMapUnitsMinus1 = 0;
    std::uint32_t frameMbsOnlyFlag = 0;
    if (!reader.readUE(&widthInMbsMinus1) || !reader.readUE(&heightInMapUnitsMinus1) ||
        !reader.readBits(1, &frameMbsOnlyFlag)) {
        return std::nullopt;
    }
    if (frameMbsOnlyFlag == 0 && !reader.readBits(1, &ignored)) {
        return std::nullopt;
    }
    if (!reader.readBits(1, &ignored)) {
        return std::nullopt;
    }

    std::uint32_t cropLeft = 0;
    std::uint32_t cropRight = 0;
    std::uint32_t cropTop = 0;
    std::uint32_t cropBottom = 0;
    std::uint32_t cropPresent = 0;
    if (!reader.readBit(&cropPresent)) {
        return std::nullopt;
    }
    if (cropPresent != 0) {
        if (!reader.readUE(&cropLeft) || !reader.readUE(&cropRight) || !reader.readUE(&cropTop) ||
            !reader.readUE(&cropBottom)) {
            return std::nullopt;
        }
    }

    const std::uint32_t frameHeightInMbs = (2U - frameMbsOnlyFlag) * (heightInMapUnitsMinus1 + 1U);
    std::uint32_t cropUnitX = 1;
    std::uint32_t cropUnitY = 2U - frameMbsOnlyFlag;
    if (chromaFormatIdc == 1) {
        cropUnitX = 2;
        cropUnitY = 2 * (2U - frameMbsOnlyFlag);
    } else if (chromaFormatIdc == 2) {
        cropUnitX = 2;
        cropUnitY = 2U - frameMbsOnlyFlag;
    }
    VideoSize result;
    result.width = (widthInMbsMinus1 + 1U) * 16U - (cropLeft + cropRight) * cropUnitX;
    result.height = frameHeightInMbs * 16U - (cropTop + cropBottom) * cropUnitY;
    if (result.width == 0 || result.height == 0) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::pair<std::size_t, std::size_t>> findStartCode(const Bytes& bytes,
                                                                 std::size_t offset) {
    for (std::size_t i = offset; i + 3 <= bytes.size(); ++i) {
        if (bytes[i] == 0x00 && bytes[i + 1] == 0x00) {
            if (bytes[i + 2] == 0x01) {
                return std::make_pair(i, 3U);
            }
            if (i + 4 <= bytes.size() && bytes[i + 2] == 0x00 && bytes[i + 3] == 0x01) {
                return std::make_pair(i, 4U);
            }
        }
    }
    return std::nullopt;
}

struct H264AccessUnit {
    Bytes bytes;
    bool keyframe = false;
    std::optional<VideoSize> spsSize;
    std::uint32_t nalCount = 0;
    std::string nalSummary;
};

bool isH264VclNal(Byte nalType) {
    return nalType >= 1 && nalType <= 5;
}

const char* h264NalTypeName(Byte nalType) {
    switch (nalType) {
    case 1:
        return "non-idr";
    case 5:
        return "idr";
    case 6:
        return "sei";
    case 7:
        return "sps";
    case 8:
        return "pps";
    case 9:
        return "aud";
    default:
        return "nal";
    }
}

std::optional<std::uint32_t> h264FirstMbInSlice(const Byte* nal, std::size_t size) {
    if (size < 2) {
        return std::nullopt;
    }
    auto rbsp = rbspFromNal(nal + 1, size - 1);
    BitReader reader(rbsp);
    std::uint32_t firstMb = 0;
    if (!reader.readUE(&firstMb)) {
        return std::nullopt;
    }
    return firstMb;
}

H264AccessUnit inspectH264AccessUnit(const Bytes& bytes) {
    H264AccessUnit unit;

    const auto starts = [&bytes]() {
        std::vector<std::pair<std::size_t, std::size_t>> result;
        std::size_t offset = 0;
        while (auto start = findStartCode(bytes, offset)) {
            result.push_back(*start);
            offset = start->first + start->second;
        }
        return result;
    }();
    if (starts.empty()) {
        unit.nalSummary = "no-annexb-start-code";
        return unit;
    }

    std::ostringstream summary;
    constexpr std::size_t kMaxSummaryNals = 10;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const auto nalStart = starts[i].first + starts[i].second;
        const auto nalEnd = i + 1 < starts.size() ? starts[i + 1].first : bytes.size();
        if (nalStart >= nalEnd || nalEnd > bytes.size()) {
            continue;
        }
        const auto nalSize = nalEnd - nalStart;
        const auto nalType = static_cast<Byte>(bytes[nalStart] & 0x1FU);
        ++unit.nalCount;
        unit.keyframe = unit.keyframe || nalType == 5;
        if (nalType == 7) {
            if (auto size = parseSps(bytes.data() + nalStart, nalSize)) {
                unit.spsSize = size;
            }
        }

        if (unit.nalCount <= kMaxSummaryNals) {
            if (unit.nalCount > 1) {
                summary << ",";
            }
            summary << static_cast<int>(nalType) << ":" << h264NalTypeName(nalType)
                    << "(" << nalSize;
            if (isH264VclNal(nalType)) {
                const auto firstMb = h264FirstMbInSlice(bytes.data() + nalStart, nalSize);
                if (firstMb.has_value()) {
                    summary << ",mb=" << *firstMb;
                } else {
                    summary << ",mb=?";
                }
            }
            summary << ")";
        }
    }
    if (unit.nalCount > kMaxSummaryNals) {
        summary << ",...";
    }
    unit.nalSummary = summary.str();
    return unit;
}

class H264AccessUnitAssembler {
public:
    void reset() {
        _buffer.clear();
        _currentAu.clear();
        _currentHasVcl = false;
        _currentKeyframe = false;
        _currentSpsSize.reset();
        _currentNalCount = 0;
    }

    std::vector<H264AccessUnit> pushChunk(const Bytes& chunk) {
        std::vector<H264AccessUnit> units;
        if (chunk.empty()) {
            return units;
        }

        _buffer.insert(_buffer.end(), chunk.begin(), chunk.end());
        discardBeforeFirstStartCode();

        const auto starts = startCodes();
        if (starts.size() < 2) {
            trimIfHopeless();
            return units;
        }

        for (std::size_t i = 0; i + 1 < starts.size(); ++i) {
            appendCompleteNal(starts[i].first, starts[i].second, starts[i + 1].first, &units);
        }

        _buffer.erase(_buffer.begin(),
                      _buffer.begin() + static_cast<std::ptrdiff_t>(starts.back().first));
        flushBeforePendingNalIfBoundary(&units);
        trimIfHopeless();
        return units;
    }

    std::size_t pendingBytes() const {
        return _buffer.size() + _currentAu.size();
    }

private:
    static bool isVclNal(Byte nalType) {
        return isH264VclNal(nalType);
    }

    static bool isBoundaryNonVcl(Byte nalType) {
        return nalType == 6 || nalType == 7 || nalType == 8 || nalType == 9;
    }

    static std::optional<std::uint32_t> firstMbInSlice(const Byte* nal, std::size_t size) {
        if (size < 2) {
            return std::nullopt;
        }
        return h264FirstMbInSlice(nal, size);
    }

    std::vector<std::pair<std::size_t, std::size_t>> startCodes() const {
        std::vector<std::pair<std::size_t, std::size_t>> result;
        std::size_t offset = 0;
        while (auto start = findStartCode(_buffer, offset)) {
            result.push_back(*start);
            offset = start->first + start->second;
        }
        return result;
    }

    void discardBeforeFirstStartCode() {
        const auto first = findStartCode(_buffer, 0);
        if (!first.has_value()) {
            trimIfHopeless();
            return;
        }
        if (first->first != 0) {
            _buffer.erase(_buffer.begin(),
                          _buffer.begin() + static_cast<std::ptrdiff_t>(first->first));
        }
    }

    bool nalStartsNewAccessUnit(Byte nalType, const Byte* nal, std::size_t size) const {
        if (!_currentHasVcl) {
            return false;
        }
        if (isBoundaryNonVcl(nalType)) {
            return true;
        }
        if (!isVclNal(nalType)) {
            return false;
        }
        const auto firstMb = firstMbInSlice(nal, size);
        return firstMb.has_value() && *firstMb == 0;
    }

    void appendCompleteNal(std::size_t startCodeOffset,
                           std::size_t startCodeSize,
                           std::size_t nalEnd,
                           std::vector<H264AccessUnit>* units) {
        const auto nalStart = startCodeOffset + startCodeSize;
        if (nalStart >= nalEnd || nalEnd > _buffer.size()) {
            return;
        }

        const auto nalType = static_cast<Byte>(_buffer[nalStart] & 0x1FU);
        if (nalStartsNewAccessUnit(nalType, _buffer.data() + nalStart, nalEnd - nalStart)) {
            flushCurrent(units);
        }

        _currentAu.insert(_currentAu.end(),
                          _buffer.begin() + static_cast<std::ptrdiff_t>(startCodeOffset),
                          _buffer.begin() + static_cast<std::ptrdiff_t>(nalEnd));
        ++_currentNalCount;

        if (isVclNal(nalType)) {
            _currentHasVcl = true;
            _currentKeyframe = _currentKeyframe || nalType == 5;
        } else if (nalType == 7) {
            if (auto size = parseSps(_buffer.data() + nalStart, nalEnd - nalStart)) {
                _currentSpsSize = size;
            }
        }
    }

    void flushBeforePendingNalIfBoundary(std::vector<H264AccessUnit>* units) {
        const auto pendingStart = findStartCode(_buffer, 0);
        if (!pendingStart.has_value() || pendingStart->first != 0) {
            return;
        }
        const auto nalStart = pendingStart->first + pendingStart->second;
        if (nalStart >= _buffer.size()) {
            return;
        }
        const auto nalType = static_cast<Byte>(_buffer[nalStart] & 0x1FU);
        if (nalStartsNewAccessUnit(nalType, _buffer.data() + nalStart, _buffer.size() - nalStart)) {
            flushCurrent(units);
        }
    }

    void flushCurrent(std::vector<H264AccessUnit>* units) {
        if (!_currentHasVcl || _currentAu.empty()) {
            return;
        }
        H264AccessUnit unit;
        unit.bytes = std::move(_currentAu);
        unit.keyframe = _currentKeyframe;
        unit.spsSize = _currentSpsSize;
        unit.nalCount = _currentNalCount;
        unit.nalSummary = inspectH264AccessUnit(unit.bytes).nalSummary;
        units->push_back(std::move(unit));

        _currentAu.clear();
        _currentHasVcl = false;
        _currentKeyframe = false;
        _currentSpsSize.reset();
        _currentNalCount = 0;
    }

    void trimIfHopeless() {
        constexpr std::size_t kMaxBufferedBytes = 4 * 1024 * 1024;
        if (_buffer.size() <= kMaxBufferedBytes) {
            return;
        }
        const auto keep = std::min<std::size_t>(_buffer.size(), 256 * 1024);
        _buffer.erase(_buffer.begin(),
                      _buffer.end() - static_cast<std::ptrdiff_t>(keep));
        discardBeforeFirstStartCode();
    }

    Bytes _buffer;
    Bytes _currentAu;
    bool _currentHasVcl = false;
    bool _currentKeyframe = false;
    std::optional<VideoSize> _currentSpsSize;
    std::uint32_t _currentNalCount = 0;
};
