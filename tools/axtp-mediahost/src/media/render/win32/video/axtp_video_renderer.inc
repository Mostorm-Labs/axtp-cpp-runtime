class AxtpVideoRenderer {
public:
    explicit AxtpVideoRenderer(LogFn log)
        : _log(std::move(log)), _presenter(_log), _decoder(_log) {}

    ~AxtpVideoRenderer() {
        stop();
    }

    bool start(RenderBackend backend,
               H264FeedMode feedMode,
               bool overlayEnabled,
               std::function<bool()> toggleMuted,
               std::function<bool()> isMuted) {
        _backend = backend;
        _feedMode = feedMode;
        _overlayEnabled = overlayEnabled;
        _toggleMuted = std::move(toggleMuted);
        _isMuted = std::move(isMuted);
        _muted.store(_isMuted ? _isMuted() : false);
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

    void setMuted(bool muted) {
        _muted.store(muted);
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            hwnd = _mainHwnd;
        }
        if (hwnd != nullptr) {
            PostMessageW(hwnd, kUpdateMutedMessage, muted ? 1 : 0, 0);
        }
    }

    bool consumeStopCastingRequested() {
        return _stopCastingRequested.exchange(false);
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
        _hasPresentedVideo.store(false);
        _stopCastingRequested.store(false);
        _mainHwnd = nullptr;
        _videoHwnd = nullptr;
        _windowReady = false;
    }

    bool running() const {
        return _running.load();
    }

    void resetDecoder(std::string_view reason) {
        std::lock_guard<std::mutex> lock(_streamMutex);
        _assembler.reset();
        _decoder.reset();
        _firstDecodedLogged = false;
        _waitingLogged = false;
        logLine(_log, std::string("renderer video decoder reset reason=") + std::string(reason));
    }

    void setStatusText(std::string text) {
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _statusText = std::move(text);
            hwnd = _videoHwnd;
        }
        if (hwnd != nullptr) {
            if (_backend == RenderBackend::SelfTest || !_hasPresentedVideo.load()) {
                InvalidateRect(hwnd, nullptr, TRUE);
            }
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
        _videoChunkCount = 0;
        _accessUnitCount = 0;
        _decodedSampleCount = 0;
        _presentCount.store(0);
        _hasPresentedVideo.store(false);
        std::ostringstream out;
        out << "renderer init: video stream opened streamId=" << toHexU32(info.streamId)
            << " codec=" << info.codec << " size=" << _width << "x" << _height
            << " h264FeedMode=" << h264FeedModeName(_feedMode);
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
        ++_videoChunkCount;
        if (_feedMode == H264FeedMode::StreamChunk) {
            const auto unit = inspectH264AccessUnit(stream.data);
            if (shouldLogFrameCount(_videoChunkCount) || unit.keyframe || unit.nalCount == 0) {
                std::ostringstream out;
                out << "renderer video chunk streamId=" << toHexU32(stream.streamId)
                    << " seq=" << stream.seqId
                    << " chunkIndex=" << _videoChunkCount
                    << " bytes=" << stream.data.size()
                    << " feedMode=stream-chunk"
                    << " nalCount=" << unit.nalCount
                    << " keyframe=" << (unit.keyframe ? "true" : "false")
                    << " nals=" << unit.nalSummary
                    << " cursor=" << stream.cursor;
                logLine(_log, out.str());
            }
            if (unit.nalCount == 0) {
                logLine(_log,
                        "renderer video chunk dropped: missing Annex-B start code"
                            " streamId=" + toHexU32(stream.streamId) +
                            " seq=" + std::to_string(stream.seqId) +
                            " bytes=" + std::to_string(stream.data.size()) +
                            " cursor=" + std::to_string(stream.cursor));
                return;
            }
            processH264Sample(stream.data, unit, stream.cursor, "stream-chunk");
            return;
        }

        auto units = _assembler.pushChunk(stream.data);
        if (shouldLogFrameCount(_videoChunkCount)) {
            std::ostringstream out;
            out << "renderer video chunk streamId=" << toHexU32(stream.streamId)
                << " seq=" << stream.seqId
                << " chunkIndex=" << _videoChunkCount
                << " bytes=" << stream.data.size()
                << " feedMode=annexb-au"
                << " accessUnits=" << units.size()
                << " pendingBytes=" << _assembler.pendingBytes()
                << " cursor=" << stream.cursor;
            logLine(_log, out.str());
        }
        if (units.empty()) {
            if (!_waitingLogged) {
                _waitingLogged = true;
                logLine(_log, "waiting for SPS/keyframe before H264 decode");
            }
            return;
        }
        for (const auto& unit : units) {
            processH264Sample(unit.bytes, unit, stream.cursor, "annexb-au");
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
            _hasPresentedVideo.store(false);
            setStatusText("Casting stopped. AXTP session stays connected.");
        }
    }

private:
    static bool shouldLogFrameCount(std::uint64_t count) {
        return count <= 50 || (count % 100) == 0;
    }

    void processH264Sample(const Bytes& bytes,
                           const H264AccessUnit& unit,
                           std::uint64_t cursor,
                           std::string_view source) {
        ++_accessUnitCount;
        if (shouldLogFrameCount(_accessUnitCount)) {
            std::ostringstream out;
            out << "renderer video sample index=" << _accessUnitCount
                << " source=" << source
                << " bytes=" << bytes.size()
                << " nalCount=" << unit.nalCount
                << " keyframe=" << (unit.keyframe ? "true" : "false")
                << " nals=" << unit.nalSummary;
            logLine(_log, out.str());
        }
        if (unit.spsSize.has_value()) {
            _width = unit.spsSize->width;
            _height = unit.spsSize->height;
            std::ostringstream out;
            out << "renderer video SPS size=" << _width << "x" << _height;
            if (unit.spsSize->codedWidth != 0 && unit.spsSize->codedHeight != 0 &&
                (unit.spsSize->codedWidth != _width || unit.spsSize->codedHeight != _height)) {
                out << " coded=" << unit.spsSize->codedWidth << "x" << unit.spsSize->codedHeight;
            }
            logLine(_log, out.str());
        }
        if (_width == 0 || _height == 0) {
            if (!_waitingLogged) {
                _waitingLogged = true;
                logLine(_log, "waiting for SPS/keyframe before H264 decode");
            }
            return;
        }
        if (!_decoder.initialize(_presenter.deviceManager(), _width, _height)) {
            return;
        }
        _decoder.processAccessUnit(bytes, cursor, [this](IMFSample* sample) {
            ++_decodedSampleCount;
            if (!_firstDecodedLogged) {
                _firstDecodedLogged = true;
                logLine(_log, "first decoded video sample");
            } else if (shouldLogFrameCount(_decodedSampleCount)) {
                logLine(_log, "decoded video samples=" + std::to_string(_decodedSampleCount));
            }
            if (!isDxgiBackedSample(sample)) {
                logLine(_log,
                        "decoded video sample is not DXGI-backed; mf-d3d11 path requires "
                        "DXVA/NV12");
                return;
            }
            if (_presenter.present(sample, _width, _height)) {
                const auto presented = _presentCount.fetch_add(1) + 1;
                _hasPresentedVideo.store(true);
                if (!_firstPresentLogged) {
                    _firstPresentLogged = true;
                    logLine(_log, "first present");
                } else if (shouldLogFrameCount(presented)) {
                    logLine(_log, "presented video frames=" + std::to_string(presented));
                }
            }
        });
    }

    void requestStopCasting() {
        if (!_stopCastingRequested.exchange(true)) {
            logLine(_log, "renderer UI: stop casting requested; AXTP session stays open");
        }
        _hasPresentedVideo.store(false);
        setStatusText("Stopping current casting streams...");
    }

    void toggleMuteFromUi() {
        bool muted = !_muted.load();
        if (_toggleMuted) {
            muted = _toggleMuted();
        } else {
            _muted.store(muted);
        }
        _muted.store(muted);
        _controls.updateMuted(muted);
        logLine(_log, std::string("renderer UI: audio ") + (muted ? "muted" : "unmuted"));
    }

    void toggleMaximizeRestore() {
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            hwnd = _mainHwnd;
        }
        if (hwnd == nullptr) {
            return;
        }
        if (IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        } else {
            ShowWindow(hwnd, SW_MAXIMIZE);
        }
        _controls.updateMaximized(IsZoomed(hwnd));
    }

    void updateFpsOverlay() {
        if (!_overlayEnabled) {
            return;
        }
        const auto now = GetTickCount64();
        const auto count = _presentCount.load();
        const auto elapsed = now > _lastFpsTick ? now - _lastFpsTick : 0;
        double fps = 0.0;
        if (elapsed > 0) {
            fps = static_cast<double>(count - _lastFpsCount) * 1000.0 /
                  static_cast<double>(elapsed);
        }
        _lastFpsCount = count;
        _lastFpsTick = now;
        _stats.setFps(fps, count);
    }

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
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                case kOverlayButtonMute:
                    self->toggleMuteFromUi();
                    return 0;
                case kOverlayButtonMaxRestore:
                    self->toggleMaximizeRestore();
                    return 0;
                case kOverlayButtonStop:
                    self->requestStopCasting();
                    return 0;
                default:
                    break;
                }
            }
            return 0;
        case WM_SIZE:
            self->resizeChild();
            self->_presenter.resize();
            return 0;
        case WM_TIMER:
            if (wParam == kFpsTimerId) {
                self->updateFpsOverlay();
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case kUpdateMutedMessage:
            self->_muted.store(wParam != 0);
            self->_controls.updateMuted(wParam != 0);
            return 0;
        case WM_GETMINMAXINFO:
            if (lParam != 0) {
                auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
                minMaxInfo->ptMinTrackSize.x = 480;
                minMaxInfo->ptMinTrackSize.y = 270;
                HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO monitorInfo{};
                monitorInfo.cbSize = sizeof(monitorInfo);
                if (GetMonitorInfoW(monitor, &monitorInfo)) {
                    minMaxInfo->ptMaxPosition.x = monitorInfo.rcMonitor.left;
                    minMaxInfo->ptMaxPosition.y = monitorInfo.rcMonitor.top;
                    minMaxInfo->ptMaxSize.x =
                        monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
                    minMaxInfo->ptMaxSize.y =
                        monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
                }
            }
            return 0;
        case WM_NCHITTEST:
            return self->hitTestBorderlessWindow(hwnd, POINT{GET_X_LPARAM(lParam),
                                                             GET_Y_LPARAM(lParam)});
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (IsZoomed(hwnd)) {
                    ShowWindow(hwnd, SW_RESTORE);
                    self->_controls.updateMaximized(false);
                } else {
                    self->requestStopCasting();
                }
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            KillTimer(hwnd, kFpsTimerId);
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
        case WM_NCHITTEST:
            return HTTRANSPARENT;
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

        HWND mainHwnd = CreateWindowExW(WS_EX_APPWINDOW,
                                        mainClass,
                                        L"AXTP MediaHost Renderer",
                                        WS_POPUP | WS_CLIPCHILDREN,
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
                                         WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
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

        if (_backend == RenderBackend::SelfTest) {
            SetTimer(videoHwnd, kSelfTestTimerId, 33, nullptr);
        }
        if (_overlayEnabled) {
            if (!_controls.create(mainHwnd, instance)) {
                logLine(_log, "renderer overlay: controls window creation failed");
            } else {
                _controls.updateMuted(_muted.load());
            }
            if (!_stats.create(mainHwnd, instance)) {
                logLine(_log, "renderer overlay: FPS window creation failed");
            }
            _lastFpsTick = GetTickCount64();
            SetTimer(mainHwnd, kFpsTimerId, 1000, nullptr);
        }
        resizeChild();
        ShowWindow(mainHwnd, SW_SHOW);
        UpdateWindow(mainHwnd);
        logLine(_log, "renderer init: Win32 main/video child windows ready");

        MSG message = {};
        while (_running.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (videoHwnd != nullptr && _backend == RenderBackend::SelfTest) {
            KillTimer(videoHwnd, kSelfTestTimerId);
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
        if (_overlayEnabled) {
            _controls.layout(rect);
            _controls.updateMaximized(IsZoomed(mainHwnd));
            _stats.layout();
        }
    }

    LRESULT hitTestBorderlessWindow(HWND hwnd, POINT screenPoint) const {
        if (IsZoomed(hwnd)) {
            return HTCLIENT;
        }
        RECT window{};
        GetWindowRect(hwnd, &window);
        constexpr int border = 8;
        const bool left = screenPoint.x >= window.left && screenPoint.x < window.left + border;
        const bool right = screenPoint.x <= window.right && screenPoint.x > window.right - border;
        const bool top = screenPoint.y >= window.top && screenPoint.y < window.top + border;
        const bool bottom =
            screenPoint.y <= window.bottom && screenPoint.y > window.bottom - border;
        if (top && left) {
            return HTTOPLEFT;
        }
        if (top && right) {
            return HTTOPRIGHT;
        }
        if (bottom && left) {
            return HTBOTTOMLEFT;
        }
        if (bottom && right) {
            return HTBOTTOMRIGHT;
        }
        if (left) {
            return HTLEFT;
        }
        if (right) {
            return HTRIGHT;
        }
        if (top) {
            return HTTOP;
        }
        if (bottom) {
            return HTBOTTOM;
        }
        return HTCAPTION;
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT paintStruct = {};
        HDC dc = BeginPaint(hwnd, &paintStruct);
        RECT client = {};
        GetClientRect(hwnd, &client);
        if (_backend == RenderBackend::MfD3d11 && _hasPresentedVideo.load()) {
            EndPaint(hwnd, &paintStruct);
            return;
        }

        if (_backend == RenderBackend::SelfTest) {
            ScopedGdiObject black(CreateSolidBrush(RGB(8, 10, 12)));
            FillRect(dc, &client, static_cast<HBRUSH>(black.object));
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
        } else {
            std::string text;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                text = _statusText;
            }
            drawReceiverIdleScreen(dc, client, text);
        }
        EndPaint(hwnd, &paintStruct);
    }

    LogFn _log;
    RenderBackend _backend = RenderBackend::None;
    H264FeedMode _feedMode = H264FeedMode::StreamChunk;
    std::atomic_bool _running{false};
    std::atomic_bool _hasPresentedVideo{false};
    std::atomic_bool _stopCastingRequested{false};
    std::atomic_bool _muted{false};
    std::thread _thread;
    mutable std::mutex _mutex;
    std::condition_variable _readyCv;
    HWND _mainHwnd = nullptr;
    HWND _videoHwnd = nullptr;
    bool _windowReady = false;
    std::string _statusText;
    bool _overlayEnabled = true;
    OverlayControls _controls;
    StatsOverlay _stats;
    std::function<bool()> _toggleMuted;
    std::function<bool()> _isMuted;
    std::uint64_t _lastFpsCount = 0;
    ULONGLONG _lastFpsTick = 0;

    std::mutex _streamMutex;
    std::uint32_t _streamId = 0;
    std::uint32_t _width = 0;
    std::uint32_t _height = 0;
    bool _firstChunkLogged = false;
    bool _firstDecodedLogged = false;
    bool _firstPresentLogged = false;
    bool _waitingLogged = false;
    std::uint64_t _videoChunkCount = 0;
    std::uint64_t _accessUnitCount = 0;
    std::uint64_t _decodedSampleCount = 0;
    std::atomic<std::uint64_t> _presentCount{0};
    H264AccessUnitAssembler _assembler;
    D3DVideoPresenter _presenter;
    H264MftDecoder _decoder;
};
