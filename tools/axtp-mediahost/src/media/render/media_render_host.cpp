#include "media/render/media_render_host.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <audioclient.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace axtp::mediahost {
namespace {

using Microsoft::WRL::ComPtr;

constexpr REFERENCE_TIME kAudioBufferDuration = 10 * 1000 * 1000;

std::string hresultToString(HRESULT hr) {
    char message[256] = {};
    const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD written = FormatMessageA(flags,
                                         nullptr,
                                         static_cast<DWORD>(hr),
                                         0,
                                         message,
                                         static_cast<DWORD>(sizeof(message)),
                                         nullptr);
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    if (written != 0) {
        out << " " << message;
    }
    return out.str();
}

void logLine(const LogFn& log, std::string_view line) {
    if (log) {
        log(line);
    }
}

bool failed(HRESULT hr, const LogFn& log, std::string_view stage) {
    if (SUCCEEDED(hr)) {
        return false;
    }
    std::string line(stage);
    line += " failed: ";
    line += hresultToString(hr);
    logLine(log, line);
    return true;
}

std::wstring widen(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), required);
    return output;
}

bool isDxgiBackedSample(IMFSample* sample) {
    if (sample == nullptr) {
        return false;
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) {
        return false;
    }
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    return SUCCEEDED(buffer.As(&dxgiBuffer));
}

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
};

class H264AccessUnitAssembler {
public:
    void reset() {
        _parameterSets.clear();
    }

    std::vector<H264AccessUnit> pushChunk(const Bytes& chunk) {
        std::vector<H264AccessUnit> units;
        if (chunk.empty()) {
            return units;
        }

        bool hasVcl = false;
        bool keyframe = false;
        std::optional<VideoSize> parsedSize;
        auto start = findStartCode(chunk, 0);
        while (start.has_value()) {
            const auto nalStart = start->first + start->second;
            const auto next = findStartCode(chunk, nalStart);
            const auto nalEnd = next.has_value() ? next->first : chunk.size();
            if (nalStart < nalEnd) {
                const auto nalType = chunk[nalStart] & 0x1FU;
                if (nalType >= 1 && nalType <= 5) {
                    hasVcl = true;
                    keyframe = keyframe || nalType == 5;
                } else if (nalType == 7) {
                    parsedSize = parseSps(chunk.data() + nalStart, nalEnd - nalStart);
                }
            }
            if (!next.has_value()) {
                break;
            }
            start = next;
        }

        if (!hasVcl) {
            _parameterSets.insert(_parameterSets.end(), chunk.begin(), chunk.end());
            if (_parameterSets.size() > 1024 * 1024) {
                _parameterSets.erase(_parameterSets.begin(), _parameterSets.end() - (256 * 1024));
            }
            return units;
        }

        H264AccessUnit unit;
        unit.bytes.reserve(_parameterSets.size() + chunk.size());
        unit.bytes.insert(unit.bytes.end(), _parameterSets.begin(), _parameterSets.end());
        unit.bytes.insert(unit.bytes.end(), chunk.begin(), chunk.end());
        unit.keyframe = keyframe;
        unit.spsSize = parsedSize;
        _parameterSets.clear();
        units.push_back(std::move(unit));
        return units;
    }

private:
    Bytes _parameterSets;
};

struct AdtsConfig {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
};

struct AdtsFrame {
    Bytes bytes;
    AdtsConfig config;
};

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
            const std::uint32_t channelConfig =
                ((_buffer[2] & 0x01U) << 2U) | ((_buffer[3] & 0xC0U) >> 6U);
            const std::uint32_t frameLength = ((_buffer[3] & 0x03U) << 11U) |
                                              (static_cast<std::uint32_t>(_buffer[4]) << 3U) |
                                              ((_buffer[5] & 0xE0U) >> 5U);
            if (frameLength < 7) {
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

class D3DVideoPresenter {
public:
    explicit D3DVideoPresenter(LogFn log) : _log(std::move(log)) {}

    bool initialize(HWND hwnd) {
        std::lock_guard<std::mutex> lock(_mutex);
        _hwnd = hwnd;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        static constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(nullptr,
                                       D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr,
                                       flags,
                                       kFeatureLevels,
                                       static_cast<UINT>(std::size(kFeatureLevels)),
                                       D3D11_SDK_VERSION,
                                       &_device,
                                       &featureLevel,
                                       &_context);
        if (FAILED(hr)) {
            logLine(_log, "D3D11 hardware device failed; trying WARP: " + hresultToString(hr));
            hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_WARP,
                                   nullptr,
                                   flags,
                                   kFeatureLevels,
                                   static_cast<UINT>(std::size(kFeatureLevels)),
                                   D3D11_SDK_VERSION,
                                   &_device,
                                   &featureLevel,
                                   &_context);
        }
        if (failed(hr, _log, "D3D11CreateDevice")) {
            return false;
        }

        UINT resetToken = 0;
        hr = MFCreateDXGIDeviceManager(&resetToken, &_deviceManager);
        if (failed(hr, _log, "MFCreateDXGIDeviceManager")) {
            return false;
        }
        _resetToken = resetToken;
        hr = _deviceManager->ResetDevice(_device.Get(), _resetToken);
        if (failed(hr, _log, "IMFDXGIDeviceManager::ResetDevice")) {
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = _device.As(&dxgiDevice);
        if (failed(hr, _log, "ID3D11Device as IDXGIDevice")) {
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(&adapter);
        if (failed(hr, _log, "IDXGIDevice::GetAdapter")) {
            return false;
        }
        hr = adapter->GetParent(IID_PPV_ARGS(&_factory));
        if (failed(hr, _log, "IDXGIAdapter::GetParent(IDXGIFactory2)")) {
            return false;
        }
        hr = _device.As(&_videoDevice);
        if (failed(hr, _log, "ID3D11Device as ID3D11VideoDevice")) {
            return false;
        }
        hr = _context.As(&_videoContext);
        if (failed(hr, _log, "ID3D11DeviceContext as ID3D11VideoContext")) {
            return false;
        }

        RECT rect = {};
        GetClientRect(_hwnd, &rect);
        const auto width = std::max<UINT>(1, static_cast<UINT>(rect.right - rect.left));
        const auto height = std::max<UINT>(1, static_cast<UINT>(rect.bottom - rect.top));

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Stereo = FALSE;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        hr = _factory->CreateSwapChainForHwnd(
            _device.Get(), _hwnd, &desc, nullptr, nullptr, &_swapChain);
        if (failed(hr, _log, "IDXGIFactory2::CreateSwapChainForHwnd")) {
            return false;
        }
        _outputWidth = width;
        _outputHeight = height;
        logLine(_log, "renderer init: D3D11/DXGI ready");
        return true;
    }

    IMFDXGIDeviceManager* deviceManager() const {
        return _deviceManager.Get();
    }

    void resize() {
        std::lock_guard<std::mutex> lock(_mutex);
        resizeLocked();
    }

    bool present(IMFSample* sample) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_swapChain == nullptr || _videoDevice == nullptr || _videoContext == nullptr) {
            logLine(_log, "renderer present failed: D3D11 swap chain is not ready");
            return false;
        }

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = sample->GetBufferByIndex(0, &buffer);
        if (failed(hr, _log, "IMFSample::GetBufferByIndex")) {
            return false;
        }
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        hr = buffer.As(&dxgiBuffer);
        if (FAILED(hr)) {
            logLine(_log,
                    "renderer decode produced system-memory sample; DXGI-backed NV12 is "
                    "required for mf-d3d11");
            return false;
        }

        ComPtr<ID3D11Texture2D> inputTexture;
        hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&inputTexture));
        if (failed(hr, _log, "IMFDXGIBuffer::GetResource")) {
            return false;
        }
        UINT subresource = 0;
        dxgiBuffer->GetSubresourceIndex(&subresource);

        D3D11_TEXTURE2D_DESC inputDesc = {};
        inputTexture->GetDesc(&inputDesc);
        if (!ensureVideoProcessorLocked(inputDesc.Width, inputDesc.Height)) {
            return false;
        }

        ComPtr<ID3D11Texture2D> backBuffer;
        hr = _swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (failed(hr, _log, "IDXGISwapChain1::GetBuffer")) {
            return false;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
        inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDesc.Texture2D.MipSlice = 0;
        inputViewDesc.Texture2D.ArraySlice = subresource;
        ComPtr<ID3D11VideoProcessorInputView> inputView;
        hr = _videoDevice->CreateVideoProcessorInputView(
            inputTexture.Get(), _processorEnumerator.Get(), &inputViewDesc, &inputView);
        if (failed(hr, _log, "CreateVideoProcessorInputView")) {
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
        outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputViewDesc.Texture2D.MipSlice = 0;
        ComPtr<ID3D11VideoProcessorOutputView> outputView;
        hr = _videoDevice->CreateVideoProcessorOutputView(
            backBuffer.Get(), _processorEnumerator.Get(), &outputViewDesc, &outputView);
        if (failed(hr, _log, "CreateVideoProcessorOutputView")) {
            return false;
        }

        RECT dest = {};
        GetClientRect(_hwnd, &dest);
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.pInputSurface = inputView.Get();
        _videoContext->VideoProcessorSetStreamDestRect(_processor.Get(), 0, TRUE, &dest);
        hr = _videoContext->VideoProcessorBlt(_processor.Get(), outputView.Get(), 0, 1, &stream);
        if (failed(hr, _log, "ID3D11VideoContext::VideoProcessorBlt")) {
            return false;
        }
        hr = _swapChain->Present(1, 0);
        if (failed(hr, _log, "IDXGISwapChain1::Present")) {
            return false;
        }
        return true;
    }

private:
    void resizeLocked() {
        if (_swapChain == nullptr || _hwnd == nullptr) {
            return;
        }
        RECT rect = {};
        GetClientRect(_hwnd, &rect);
        const auto width = std::max<UINT>(1, static_cast<UINT>(rect.right - rect.left));
        const auto height = std::max<UINT>(1, static_cast<UINT>(rect.bottom - rect.top));
        if (width == _outputWidth && height == _outputHeight) {
            return;
        }
        _processor.Reset();
        _processorEnumerator.Reset();
        HRESULT hr = _swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            logLine(_log, "DXGI ResizeBuffers failed: " + hresultToString(hr));
            return;
        }
        _outputWidth = width;
        _outputHeight = height;
    }

    bool ensureVideoProcessorLocked(UINT inputWidth, UINT inputHeight) {
        resizeLocked();
        if (_processor != nullptr && _processorEnumerator != nullptr && _inputWidth == inputWidth &&
            _inputHeight == inputHeight) {
            return true;
        }

        _processor.Reset();
        _processorEnumerator.Reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = inputWidth;
        content.InputHeight = inputHeight;
        content.OutputWidth = _outputWidth;
        content.OutputHeight = _outputHeight;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        content.InputFrameRate.Numerator = 60;
        content.InputFrameRate.Denominator = 1;
        content.OutputFrameRate.Numerator = 60;
        content.OutputFrameRate.Denominator = 1;

        HRESULT hr = _videoDevice->CreateVideoProcessorEnumerator(&content, &_processorEnumerator);
        if (failed(hr, _log, "CreateVideoProcessorEnumerator")) {
            return false;
        }
        hr = _videoDevice->CreateVideoProcessor(_processorEnumerator.Get(), 0, &_processor);
        if (failed(hr, _log, "CreateVideoProcessor")) {
            return false;
        }
        _inputWidth = inputWidth;
        _inputHeight = inputHeight;
        std::ostringstream out;
        out << "renderer init: D3D11 video processor created input=" << inputWidth << "x"
            << inputHeight << " output=" << _outputWidth << "x" << _outputHeight;
        logLine(_log, out.str());
        return true;
    }

    LogFn _log;
    mutable std::mutex _mutex;
    HWND _hwnd = nullptr;
    UINT _resetToken = 0;
    UINT _inputWidth = 0;
    UINT _inputHeight = 0;
    UINT _outputWidth = 0;
    UINT _outputHeight = 0;
    ComPtr<ID3D11Device> _device;
    ComPtr<ID3D11DeviceContext> _context;
    ComPtr<IMFDXGIDeviceManager> _deviceManager;
    ComPtr<IDXGIFactory2> _factory;
    ComPtr<IDXGISwapChain1> _swapChain;
    ComPtr<ID3D11VideoDevice> _videoDevice;
    ComPtr<ID3D11VideoContext> _videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> _processorEnumerator;
    ComPtr<ID3D11VideoProcessor> _processor;
};

class H264MftDecoder {
public:
    explicit H264MftDecoder(LogFn log) : _log(std::move(log)) {}

    void reset() {
        _decoder.Reset();
        _outputTypeSet = false;
        _streamingStarted = false;
        _width = 0;
        _height = 0;
    }

    bool
    initialize(IMFDXGIDeviceManager* deviceManager, std::uint32_t width, std::uint32_t height) {
        if (_decoder != nullptr && (width == 0 || width == _width) &&
            (height == 0 || height == _height)) {
            return true;
        }
        reset();
        _width = width;
        _height = height;

        HRESULT hr = CoCreateInstance(
            CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_decoder));
        if (failed(hr, _log, "CoCreateInstance(CMSH264DecoderMFT)")) {
            return false;
        }
        if (deviceManager != nullptr) {
            hr = _decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                          reinterpret_cast<ULONG_PTR>(deviceManager));
            if (FAILED(hr)) {
                logLine(_log, "H264 decoder rejected D3D manager: " + hresultToString(hr));
            }
        }

        ComPtr<IMFMediaType> inputType;
        hr = MFCreateMediaType(&inputType);
        if (failed(hr, _log, "MFCreateMediaType(video input)")) {
            return false;
        }
        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (_width != 0 && _height != 0) {
            MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, _width, _height);
        }
        hr = _decoder->SetInputType(0, inputType.Get(), 0);
        if (failed(hr, _log, "H264 decoder SetInputType")) {
            return false;
        }
        return setOutputType();
    }

    template <typename Callback>
    void processAccessUnit(const Bytes& accessUnit, std::uint64_t cursor, Callback&& onSample) {
        if (_decoder == nullptr || accessUnit.empty()) {
            return;
        }
        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateSample(&sample);
        if (failed(hr, _log, "MFCreateSample(video input)")) {
            return;
        }
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(accessUnit.size()), &buffer);
        if (failed(hr, _log, "MFCreateMemoryBuffer(video input)")) {
            return;
        }
        Byte* data = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        hr = buffer->Lock(&data, &maxLength, &currentLength);
        if (failed(hr, _log, "IMFMediaBuffer::Lock(video input)")) {
            return;
        }
        std::memcpy(data, accessUnit.data(), accessUnit.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(accessUnit.size()));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(cursor * 10ULL));

        if (!_streamingStarted) {
            _decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            _decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            _streamingStarted = true;
        }

        hr = _decoder->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) {
            logLine(_log, "H264 decoder ProcessInput failed: " + hresultToString(hr));
            return;
        }
        drainOutput(std::forward<Callback>(onSample));
    }

private:
    bool setOutputType() {
        if (_decoder == nullptr) {
            return false;
        }
        ComPtr<IMFMediaType> outputType;
        HRESULT hr = MFCreateMediaType(&outputType);
        if (failed(hr, _log, "MFCreateMediaType(video output)")) {
            return false;
        }
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (_width != 0 && _height != 0) {
            MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, _width, _height);
        }
        hr = _decoder->SetOutputType(0, outputType.Get(), 0);
        if (FAILED(hr)) {
            for (DWORD i = 0; SUCCEEDED(_decoder->GetOutputAvailableType(0, i, &outputType)); ++i) {
                GUID subtype = {};
                if (SUCCEEDED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                    subtype == MFVideoFormat_NV12 &&
                    SUCCEEDED(_decoder->SetOutputType(0, outputType.Get(), 0))) {
                    _outputTypeSet = true;
                    logLine(_log, "renderer init: H264 decoder output type NV12 selected");
                    return true;
                }
                outputType.Reset();
            }
            logLine(_log, "H264 decoder SetOutputType(NV12) failed: " + hresultToString(hr));
            return false;
        }
        _outputTypeSet = true;
        logLine(_log, "renderer init: H264 decoder output type NV12 selected");
        return true;
    }

    template <typename Callback> void drainOutput(Callback&& onSample) {
        if (!_outputTypeSet) {
            return;
        }
        MFT_OUTPUT_STREAM_INFO streamInfo = {};
        _decoder->GetOutputStreamInfo(0, &streamInfo);
        while (true) {
            ComPtr<IMFSample> outputSample;
            MFT_OUTPUT_DATA_BUFFER output = {};
            output.dwStreamID = 0;
            if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
                HRESULT hr = MFCreateSample(&outputSample);
                if (failed(hr, _log, "MFCreateSample(video output)")) {
                    return;
                }
                ComPtr<IMFMediaBuffer> outputBuffer;
                hr = MFCreateMemoryBuffer(
                    streamInfo.cbSize == 0 ? 4 * 1024 * 1024 : streamInfo.cbSize, &outputBuffer);
                if (failed(hr, _log, "MFCreateMemoryBuffer(video output)")) {
                    return;
                }
                outputSample->AddBuffer(outputBuffer.Get());
                output.pSample = outputSample.Get();
            }

            DWORD status = 0;
            HRESULT hr = _decoder->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents != nullptr) {
                output.pEvents->Release();
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                setOutputType();
                continue;
            }
            if (FAILED(hr)) {
                logLine(_log, "H264 decoder ProcessOutput failed: " + hresultToString(hr));
                return;
            }
            if (output.pSample != nullptr) {
                onSample(output.pSample);
            }
        }
    }

    LogFn _log;
    ComPtr<IMFTransform> _decoder;
    bool _outputTypeSet = false;
    bool _streamingStarted = false;
    std::uint32_t _width = 0;
    std::uint32_t _height = 0;
};

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
            << " blockAlign=" << _mixFormat->nBlockAlign;
        logLine(_log, out.str());
        return true;
    }

    WAVEFORMATEX* mixFormat() const {
        return _mixFormat.get();
    }

    void setMuted(bool muted) {
        _muted.store(muted);
    }

    void stop() {
        if (_audioClient != nullptr && _started) {
            _audioClient->Stop();
        }
        _started = false;
        _renderClient.Reset();
        _audioClient.Reset();
        _mixFormat.reset();
        _bufferFrames = 0;
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
    }

    bool initialize(const AdtsConfig& config, const WAVEFORMATEX& outputFormat) {
        if (_decoder != nullptr && _sampleRate == config.sampleRate &&
            _channels == config.channels) {
            return true;
        }
        reset();
        _sampleRate = config.sampleRate;
        _channels = config.channels;

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
        inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, _sampleRate);
        inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, _channels);
        inputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1);
        hr = _decoder->SetInputType(0, inputType.Get(), 0);
        if (failed(hr, _log, "AAC decoder SetInputType")) {
            return false;
        }

        ComPtr<IMFMediaType> outputType;
        hr = MFCreateMediaType(&outputType);
        if (failed(hr, _log, "MFCreateMediaType(AAC output)")) {
            return false;
        }
        const auto formatGuid = audioSubtypeFor(outputFormat);
        const UINT32 bitsPerSample = outputFormat.wBitsPerSample == 0
                                         ? (formatGuid == MFAudioFormat_Float ? 32 : 16)
                                         : outputFormat.wBitsPerSample;
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outputType->SetGUID(MF_MT_SUBTYPE, formatGuid);
        outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, outputFormat.nSamplesPerSec);
        outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, outputFormat.nChannels);
        outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bitsPerSample);
        outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, outputFormat.nBlockAlign);
        outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, outputFormat.nAvgBytesPerSec);
        hr = _decoder->SetOutputType(0, outputType.Get(), 0);
        if (failed(hr, _log, "AAC decoder SetOutputType(PCM mix format)")) {
            return false;
        }
        std::ostringstream out;
        out << "renderer init: AAC decoder ready input=" << _sampleRate << "Hz/" << _channels
            << "ch output=" << outputFormat.nSamplesPerSec << "Hz/" << outputFormat.nChannels
            << "ch";
        logLine(_log, out.str());
        return true;
    }

    template <typename Callback>
    void processFrame(const Bytes& frame, std::uint64_t cursor, Callback&& onPcm) {
        if (_decoder == nullptr || frame.empty()) {
            return;
        }
        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateSample(&sample);
        if (failed(hr, _log, "MFCreateSample(AAC input)")) {
            return;
        }
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), &buffer);
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
        std::memcpy(data, frame.data(), frame.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(frame.size()));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(cursor * 10ULL));

        if (!_streamingStarted) {
            _decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            _decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            _streamingStarted = true;
        }
        hr = _decoder->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) {
            logLine(_log, "AAC decoder ProcessInput failed: " + hresultToString(hr));
            return;
        }
        drainOutput(std::forward<Callback>(onPcm));
    }

private:
    static GUID audioSubtypeFor(const WAVEFORMATEX& format) {
        if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            return MFAudioFormat_Float;
        }
        if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
            if (extensible.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                return MFAudioFormat_Float;
            }
        }
        return MFAudioFormat_PCM;
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
            onPcm(data, currentLength);
            contiguous->Unlock();
        }
    }

    LogFn _log;
    ComPtr<IMFTransform> _decoder;
    bool _streamingStarted = false;
    std::uint32_t _sampleRate = 0;
    std::uint32_t _channels = 0;
};

} // namespace
} // namespace axtp::mediahost

namespace axtp::mediahost {

class AxtpAudioRenderer {
public:
    explicit AxtpAudioRenderer(LogFn log) : _log(std::move(log)), _wasapi(_log), _decoder(_log) {}

    bool start(bool muted) {
        _muted.store(muted);
        _wasapi.setMuted(muted);
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
        _parser.reset();
        _decoder.reset();
        _firstFrameLogged = false;
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
        for (const auto& frame : frames) {
            if (!_firstFrameLogged) {
                _firstFrameLogged = true;
                logLine(_log,
                        "first audio frame streamId=" + toHexU32(stream.streamId) +
                            " sampleRate=" + std::to_string(frame.config.sampleRate) +
                            " channels=" + std::to_string(frame.config.channels));
            }
            if (!_wasapi.initialize()) {
                return;
            }
            auto* mixFormat = _wasapi.mixFormat();
            if (mixFormat == nullptr) {
                return;
            }
            if (!_decoder.initialize(frame.config, *mixFormat)) {
                return;
            }
            _decoder.processFrame(
                frame.bytes, stream.cursor, [this](const Byte* data, std::size_t size) {
                    _wasapi.writePcm(data, size);
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
        }
    }

private:
    LogFn _log;
    std::mutex _mutex;
    std::atomic_bool _muted{false};
    std::uint32_t _streamId = 0;
    bool _firstFrameLogged = false;
    AdtsParser _parser;
    WasapiRenderer _wasapi;
    AacMftDecoder _decoder;
};

class AxtpVideoRenderer {
public:
    explicit AxtpVideoRenderer(LogFn log)
        : _log(std::move(log)), _presenter(_log), _decoder(_log) {}

    ~AxtpVideoRenderer() {
        stop();
    }

    bool start(RenderBackend backend) {
        _backend = backend;
        _running.store(true);
        _thread = std::thread([this]() { windowThread(); });
        std::unique_lock<std::mutex> lock(_mutex);
        _readyCv.wait(lock, [this]() { return _windowReady || !_running.load(); });
        if (!_windowReady) {
            logLine(_log, "renderer init failed: video window was not created");
            return false;
        }
        if (_backend == RenderBackend::MfD3d11 && !_presenter.initialize(_videoHwnd)) {
            logLine(_log, "renderer init failed: D3D11 video presenter unavailable");
            return false;
        }
        return true;
    }

    void stop() {
        _running.store(false);
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            hwnd = _mainHwnd;
        }
        if (hwnd != nullptr) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        if (_thread.joinable()) {
            _thread.join();
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _decoder.reset();
        _assembler.reset();
        _mainHwnd = nullptr;
        _videoHwnd = nullptr;
        _windowReady = false;
    }

    bool running() const {
        return _running.load();
    }

    void setStatusText(std::string text) {
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _statusText = std::move(text);
            hwnd = _videoHwnd;
        }
        if (hwnd != nullptr) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    void onStreamOpened(const MediaStreamInfo& info) {
        if (info.kind != MediaKind::Video) {
            return;
        }
        std::lock_guard<std::mutex> lock(_streamMutex);
        _streamId = info.streamId;
        _width = info.width;
        _height = info.height;
        _assembler.reset();
        _decoder.reset();
        _firstChunkLogged = false;
        _firstDecodedLogged = false;
        _firstPresentLogged = false;
        _waitingLogged = false;
        std::ostringstream out;
        out << "renderer init: video stream opened streamId=" << toHexU32(info.streamId)
            << " codec=" << info.codec << " size=" << _width << "x" << _height;
        logLine(_log, out.str());
        setStatusText("AXTP MediaHost video stream opened. Waiting for SPS/keyframe...");
    }

    void onStreamChunk(const StreamPayload& stream) {
        if (_backend == RenderBackend::SelfTest || _backend == RenderBackend::None) {
            return;
        }
        std::lock_guard<std::mutex> lock(_streamMutex);
        if (_streamId == 0 || stream.streamId != _streamId) {
            return;
        }
        if (!_firstChunkLogged) {
            _firstChunkLogged = true;
            logLine(_log,
                    "first video chunk streamId=" + toHexU32(stream.streamId) +
                        " bytes=" + std::to_string(stream.data.size()));
        }
        auto units = _assembler.pushChunk(stream.data);
        if (units.empty()) {
            if (!_waitingLogged) {
                _waitingLogged = true;
                logLine(_log, "waiting for SPS/keyframe before H264 decode");
            }
            return;
        }
        for (const auto& unit : units) {
            if (unit.spsSize.has_value()) {
                _width = unit.spsSize->width;
                _height = unit.spsSize->height;
                std::ostringstream out;
                out << "renderer video SPS size=" << _width << "x" << _height;
                logLine(_log, out.str());
            }
            if (_width == 0 || _height == 0) {
                if (!_waitingLogged) {
                    _waitingLogged = true;
                    logLine(_log, "waiting for SPS/keyframe before H264 decode");
                }
                continue;
            }
            if (!_decoder.initialize(_presenter.deviceManager(), _width, _height)) {
                return;
            }
            _decoder.processAccessUnit(unit.bytes, stream.cursor, [this](IMFSample* sample) {
                if (!_firstDecodedLogged) {
                    _firstDecodedLogged = true;
                    logLine(_log, "first decoded video sample");
                }
                if (!isDxgiBackedSample(sample)) {
                    logLine(_log,
                            "first decoded video sample is not DXGI-backed; "
                            "mf-d3d11 path "
                            "requires DXVA/NV12");
                    return;
                }
                if (_presenter.present(sample) && !_firstPresentLogged) {
                    _firstPresentLogged = true;
                    logLine(_log, "first present");
                }
            });
        }
    }

    void onStreamClosed(MediaKind kind, std::uint32_t streamId) {
        if (kind != MediaKind::Video) {
            return;
        }
        std::lock_guard<std::mutex> lock(_streamMutex);
        if (_streamId == streamId) {
            logLine(_log, "renderer video stream closed streamId=" + toHexU32(streamId));
            _streamId = 0;
            _assembler.reset();
            _decoder.reset();
            setStatusText("AXTP MediaHost waiting for video stream...");
        }
    }

private:
    static LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        AxtpVideoRenderer* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<AxtpVideoRenderer*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<AxtpVideoRenderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        switch (message) {
        case WM_SIZE:
            self->resizeChild();
            self->_presenter.resize();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            std::lock_guard<std::mutex> lock(self->_mutex);
            self->_mainHwnd = nullptr;
            self->_videoHwnd = nullptr;
        }
            self->_running.store(false);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    static LRESULT CALLBACK VideoWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        AxtpVideoRenderer* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<AxtpVideoRenderer*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<AxtpVideoRenderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        switch (message) {
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            self->paint(hwnd);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    void windowThread() {
        const wchar_t mainClass[] = L"AxtpMediaHostRenderWindow";
        const wchar_t videoClass[] = L"AxtpMediaHostVideoWindow";
        HINSTANCE instance = GetModuleHandleW(nullptr);

        WNDCLASSW mainWindowClass = {};
        mainWindowClass.lpfnWndProc = &AxtpVideoRenderer::MainWindowProc;
        mainWindowClass.hInstance = instance;
        mainWindowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        mainWindowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        mainWindowClass.lpszClassName = mainClass;
        RegisterClassW(&mainWindowClass);

        WNDCLASSW videoWindowClass = {};
        videoWindowClass.lpfnWndProc = &AxtpVideoRenderer::VideoWindowProc;
        videoWindowClass.hInstance = instance;
        videoWindowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        videoWindowClass.hbrBackground = reinterpret_cast<HBRUSH>(BLACK_BRUSH);
        videoWindowClass.lpszClassName = videoClass;
        RegisterClassW(&videoWindowClass);

        HWND mainHwnd = CreateWindowExW(0,
                                        mainClass,
                                        L"AXTP MediaHost Renderer",
                                        WS_OVERLAPPEDWINDOW,
                                        CW_USEDEFAULT,
                                        CW_USEDEFAULT,
                                        960,
                                        540,
                                        nullptr,
                                        nullptr,
                                        instance,
                                        this);
        if (mainHwnd == nullptr) {
            std::lock_guard<std::mutex> lock(_mutex);
            _running.store(false);
            _readyCv.notify_all();
            return;
        }
        HWND videoHwnd = CreateWindowExW(0,
                                         videoClass,
                                         L"",
                                         WS_CHILD | WS_VISIBLE,
                                         0,
                                         0,
                                         960,
                                         540,
                                         mainHwnd,
                                         nullptr,
                                         instance,
                                         this);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _mainHwnd = mainHwnd;
            _videoHwnd = videoHwnd;
            _windowReady = videoHwnd != nullptr;
            if (_statusText.empty()) {
                _statusText = _backend == RenderBackend::SelfTest
                                  ? "AXTP MediaHost self-test"
                                  : "AXTP MediaHost waiting for SPS/keyframe...";
            }
        }
        _readyCv.notify_all();
        if (videoHwnd == nullptr) {
            DestroyWindow(mainHwnd);
            return;
        }

        SetTimer(videoHwnd, 1, 33, nullptr);
        resizeChild();
        ShowWindow(mainHwnd, SW_SHOW);
        UpdateWindow(mainHwnd);
        logLine(_log, "renderer init: Win32 main/video child windows ready");

        MSG message = {};
        while (_running.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (videoHwnd != nullptr) {
            KillTimer(videoHwnd, 1);
        }
    }

    void resizeChild() {
        HWND mainHwnd = nullptr;
        HWND videoHwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            mainHwnd = _mainHwnd;
            videoHwnd = _videoHwnd;
        }
        if (mainHwnd == nullptr || videoHwnd == nullptr) {
            return;
        }
        RECT rect = {};
        GetClientRect(mainHwnd, &rect);
        MoveWindow(videoHwnd, 0, 0, rect.right - rect.left, rect.bottom - rect.top, TRUE);
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT paintStruct = {};
        HDC dc = BeginPaint(hwnd, &paintStruct);
        RECT client = {};
        GetClientRect(hwnd, &client);
        HBRUSH black = CreateSolidBrush(RGB(8, 10, 12));
        FillRect(dc, &client, black);
        DeleteObject(black);

        if (_backend == RenderBackend::SelfTest) {
            const COLORREF colors[] = {
                RGB(240, 50, 70),
                RGB(250, 190, 40),
                RGB(80, 210, 120),
                RGB(40, 190, 230),
                RGB(80, 110, 240),
                RGB(220, 90, 230),
            };
            const int width = std::max(1, static_cast<int>(client.right - client.left));
            const int height = std::max(1, static_cast<int>(client.bottom - client.top));
            const int barWidth = std::max(1, width / static_cast<int>(std::size(colors)));
            const auto phase = static_cast<int>((GetTickCount64() / 250) % std::size(colors));
            for (int i = 0; i < static_cast<int>(std::size(colors)); ++i) {
                RECT bar = {i * barWidth, 0, i == 5 ? width : (i + 1) * barWidth, height};
                HBRUSH brush = CreateSolidBrush(colors[(i + phase) % std::size(colors)]);
                FillRect(dc, &bar, brush);
                DeleteObject(brush);
            }
        }

        std::string text;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            text = _statusText;
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(245, 248, 250));
        RECT textRect = client;
        textRect.left += 20;
        textRect.top += 20;
        textRect.right -= 20;
        textRect.bottom -= 20;
        const auto wide = widen(text);
        DrawTextW(dc, wide.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
        EndPaint(hwnd, &paintStruct);
    }

    LogFn _log;
    RenderBackend _backend = RenderBackend::None;
    std::atomic_bool _running{false};
    std::thread _thread;
    mutable std::mutex _mutex;
    std::condition_variable _readyCv;
    HWND _mainHwnd = nullptr;
    HWND _videoHwnd = nullptr;
    bool _windowReady = false;
    std::string _statusText;

    std::mutex _streamMutex;
    std::uint32_t _streamId = 0;
    std::uint32_t _width = 0;
    std::uint32_t _height = 0;
    bool _firstChunkLogged = false;
    bool _firstDecodedLogged = false;
    bool _firstPresentLogged = false;
    bool _waitingLogged = false;
    H264AccessUnitAssembler _assembler;
    D3DVideoPresenter _presenter;
    H264MftDecoder _decoder;
};

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

MediaRenderHost::MediaRenderHost(LogFn log) : _log(std::move(log)) {}

MediaRenderHost::~MediaRenderHost() {
    stop();
}

bool MediaRenderHost::start(const MediaRenderHostOptions& options) {
    if (_running.load()) {
        return true;
    }
    _options = options;
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

    logLine(std::string("renderer init: backend=") + renderBackendName(_options.backend));
    if (_options.enableVideo) {
        _video = std::make_unique<AxtpVideoRenderer>(_log);
        if (!_video->start(_options.backend)) {
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
    _options.startMuted = muted;
    if (_audio != nullptr) {
        _audio->setMuted(muted);
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

} // namespace axtp::mediahost
