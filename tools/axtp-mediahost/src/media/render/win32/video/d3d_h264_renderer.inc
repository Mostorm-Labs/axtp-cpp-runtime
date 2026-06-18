void drawReceiverIdleScreen(HDC dc, const RECT& client, std::string_view statusText) {
    ScopedGdiObject background(CreateSolidBrush(RGB(8, 10, 12)));
    FillRect(dc, &client, static_cast<HBRUSH>(background.object));

    const int width = std::max<int>(1, static_cast<int>(client.right - client.left));
    const int height = std::max<int>(1, static_cast<int>(client.bottom - client.top));
    const COLORREF colors[] = {
        RGB(245, 72, 80),
        RGB(247, 184, 54),
        RGB(74, 191, 112),
        RGB(64, 166, 220),
        RGB(98, 113, 218),
    };
    const int bandHeight = std::max(8, height / 42);
    for (int i = 0; i < static_cast<int>(std::size(colors)); ++i) {
        RECT band{client.left + i * width / static_cast<int>(std::size(colors)),
                  client.top,
                  client.left + (i + 1) * width / static_cast<int>(std::size(colors)),
                  client.top + bandHeight};
        ScopedGdiObject brush(CreateSolidBrush(colors[i]));
        FillRect(dc, &band, static_cast<HBRUSH>(brush.object));
    }

    const int cardWidth = std::min(width - 48, 560);
    const int cardHeight = std::min(height - 48, 230);
    const int cardLeft = client.left + (width - cardWidth) / 2;
    const int cardTop = client.top + (height - cardHeight) / 2;
    RECT card{cardLeft, cardTop, cardLeft + cardWidth, cardTop + cardHeight};
    ScopedGdiObject cardBrush(CreateSolidBrush(RGB(17, 20, 24)));
    ScopedGdiObject cardPen(CreatePen(PS_SOLID, 1, RGB(72, 78, 86)));
    const auto oldBrush = SelectObject(dc, cardBrush.object);
    const auto oldPen = SelectObject(dc, cardPen.object);
    RoundRect(dc, card.left, card.top, card.right, card.bottom, 18, 18);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);

    const int iconCx = card.left + 70;
    const int iconCy = card.top + 74;
    RECT screen{iconCx - 34, iconCy - 24, iconCx + 34, iconCy + 20};
    ScopedGdiObject iconPen(CreatePen(PS_SOLID, 3, RGB(230, 235, 240)));
    ScopedGdiObject iconBrush(CreateSolidBrush(RGB(28, 32, 38)));
    const auto oldIconPen = SelectObject(dc, iconPen.object);
    const auto oldIconBrush = SelectObject(dc, iconBrush.object);
    RoundRect(dc, screen.left, screen.top, screen.right, screen.bottom, 8, 8);
    MoveToEx(dc, iconCx - 14, screen.bottom + 12, nullptr);
    LineTo(dc, iconCx + 14, screen.bottom + 12);
    MoveToEx(dc, iconCx, screen.bottom, nullptr);
    LineTo(dc, iconCx, screen.bottom + 12);
    SelectObject(dc, oldIconBrush);
    SelectObject(dc, oldIconPen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(244, 247, 250));
    RECT titleRect{card.left + 126, card.top + 42, card.right - 34, card.top + 84};
    const auto title = widen("AXTP MediaHost Receiver");
    DrawTextW(dc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(dc, RGB(174, 184, 195));
    RECT bodyRect{card.left + 126, card.top + 88, card.right - 34, card.bottom - 32};
    std::string body(statusText);
    if (body.empty()) {
        body = "Casting is stopped. AXTP session stays connected.";
    }
    body += "\nWaiting for the next device source event.";
    const auto wideBody = widen(body);
    DrawTextW(dc, wideBody.c_str(), -1, &bodyRect, DT_LEFT | DT_TOP | DT_WORDBREAK);

    RECT footer{client.left + 24, client.bottom - 38, client.right - 24, client.bottom - 12};
    SetTextColor(dc, RGB(100, 112, 126));
    const auto footerText = widen("Receiver idle screen");
    DrawTextW(dc, footerText.c_str(), -1, &footer, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

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
        _inputCount = 0;
        _outputPollCount = 0;
        _outputSampleCount = 0;
        _needMoreInputCount = 0;
        _streamChangeCount = 0;
        _notAcceptingCount = 0;
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
        ++_inputCount;
        const bool logInput = shouldLog(_inputCount);
        if (logInput) {
            std::ostringstream out;
            out << "H264 decoder input index=" << _inputCount
                << " bytes=" << accessUnit.size()
                << " cursor=" << cursor;
            logLine(_log, out.str());
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
        if (hr == MF_E_NOTACCEPTING) {
            ++_notAcceptingCount;
            logLine(_log,
                    "H264 decoder input backpressure: MF_E_NOTACCEPTING, draining output");
            drainOutput(onSample);
            hr = _decoder->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            logLine(_log, "H264 decoder ProcessInput failed: " + hresultToString(hr));
            return;
        }
        if (logInput) {
            logLine(_log, "H264 decoder input accepted index=" + std::to_string(_inputCount));
        }
        drainOutput(std::forward<Callback>(onSample));
    }

private:
    static bool shouldLog(std::uint64_t count) {
        return count <= 50 || (count % 100) == 0;
    }

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
            const bool callerProvidesSample =
                (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0;
            if (callerProvidesSample) {
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
            ++_outputPollCount;
            if (output.pEvents != nullptr) {
                output.pEvents->Release();
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                ++_needMoreInputCount;
                if (shouldLog(_needMoreInputCount)) {
                    std::ostringstream out;
                    out << "H264 decoder needs more input"
                        << " count=" << _needMoreInputCount
                        << " outputPoll=" << _outputPollCount;
                    logLine(_log, out.str());
                }
                return;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ++_streamChangeCount;
                std::ostringstream out;
                out << "H264 decoder stream change"
                    << " count=" << _streamChangeCount
                    << " outputPoll=" << _outputPollCount;
                logLine(_log, out.str());
                setOutputType();
                continue;
            }
            if (FAILED(hr)) {
                logLine(_log, "H264 decoder ProcessOutput failed: " + hresultToString(hr));
                return;
            }
            if (output.pSample != nullptr) {
                ++_outputSampleCount;
                if (shouldLog(_outputSampleCount)) {
                    std::ostringstream out;
                    out << "H264 decoder output sample index=" << _outputSampleCount
                        << " outputPoll=" << _outputPollCount
                        << " outputStatus=0x" << std::hex << std::uppercase
                        << output.dwStatus
                        << " transformStatus=0x" << status;
                    logLine(_log, out.str());
                }
                onSample(output.pSample);
                if (!callerProvidesSample) {
                    output.pSample->Release();
                    output.pSample = nullptr;
                }
            }
        }
    }

    LogFn _log;
    ComPtr<IMFTransform> _decoder;
    bool _outputTypeSet = false;
    bool _streamingStarted = false;
    std::uint32_t _width = 0;
    std::uint32_t _height = 0;
    std::uint64_t _inputCount = 0;
    std::uint64_t _outputPollCount = 0;
    std::uint64_t _outputSampleCount = 0;
    std::uint64_t _needMoreInputCount = 0;
    std::uint64_t _streamChangeCount = 0;
    std::uint64_t _notAcceptingCount = 0;
};
