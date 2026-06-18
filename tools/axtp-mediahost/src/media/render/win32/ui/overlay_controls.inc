RECT buttonRectForIndex(const RECT& client, int index) {
    constexpr int kButtonSize = 58;
    constexpr int kButtonGap = 18;
    constexpr int kOverlayPadding = 12;
    const int x = kOverlayPadding;
    const int y = kOverlayPadding + index * (kButtonSize + kButtonGap);
    return RECT{x, y, x + kButtonSize, y + kButtonSize};
}

RECT buttonRectForCommand(const RECT& client, int commandId) {
    switch (commandId) {
    case kOverlayButtonMute:
        return buttonRectForIndex(client, 0);
    case kOverlayButtonMaxRestore:
        return buttonRectForIndex(client, 1);
    case kOverlayButtonStop:
        return buttonRectForIndex(client, 2);
    default:
        return RECT{};
    }
}

int commandForPoint(const RECT& client, POINT point) {
    constexpr int kCommands[] = {
        kOverlayButtonMute,
        kOverlayButtonMaxRestore,
        kOverlayButtonStop,
    };
    for (const auto command : kCommands) {
        RECT rect = buttonRectForCommand(client, command);
        if (PtInRect(&rect, point)) {
            return command;
        }
    }
    return 0;
}

void updateOverlayRegion(HWND hwnd, int width, int height) {
    HRGN combined = CreateRectRgn(0, 0, 0, 0);
    if (combined == nullptr) {
        return;
    }
    RECT client{};
    client.right = width;
    client.bottom = height;
    constexpr int kCommands[] = {
        kOverlayButtonMute,
        kOverlayButtonMaxRestore,
        kOverlayButtonStop,
    };
    for (const auto command : kCommands) {
        const RECT rect = buttonRectForCommand(client, command);
        HRGN button = CreateEllipticRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1);
        if (button == nullptr) {
            continue;
        }
        CombineRgn(combined, combined, button, RGN_OR);
        DeleteObject(button);
    }
    if (SetWindowRgn(hwnd, combined, TRUE) == 0) {
        DeleteObject(combined);
    }
}

void drawButtonShell(HDC dc, const RECT& rect, bool hovered, bool pressed, bool danger) {
    COLORREF fill = RGB(22, 24, 27);
    COLORREF outline = RGB(78, 83, 90);
    if (hovered) {
        fill = danger ? RGB(54, 24, 26) : RGB(42, 45, 49);
        outline = danger ? RGB(210, 72, 72) : RGB(118, 126, 136);
    }
    if (pressed) {
        fill = danger ? RGB(84, 28, 30) : RGB(58, 63, 70);
        outline = danger ? RGB(245, 86, 86) : RGB(156, 166, 180);
    }
    ScopedGdiObject brush(CreateSolidBrush(fill));
    ScopedGdiObject pen(CreatePen(PS_SOLID, 2, outline));
    const auto oldBrush = SelectObject(dc, brush.object);
    const auto oldPen = SelectObject(dc, pen.object);
    Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void drawVolumeIcon(HDC dc, const RECT& rect, bool muted) {
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    const int cx = rect.left + w / 2;
    const int cy = rect.top + h / 2;
    ScopedGdiObject whitePen(CreatePen(PS_SOLID, 3, RGB(245, 248, 250)));
    ScopedGdiObject whiteBrush(CreateSolidBrush(RGB(245, 248, 250)));
    const auto oldPen = SelectObject(dc, whitePen.object);
    const auto oldBrush = SelectObject(dc, whiteBrush.object);

    POINT body[] = {
        {cx - 18, cy - 9},
        {cx - 9, cy - 9},
        {cx + 6, cy - 20},
        {cx + 6, cy + 20},
        {cx - 9, cy + 9},
        {cx - 18, cy + 9},
    };
    Polygon(dc, body, static_cast<int>(std::size(body)));
    if (muted) {
        MoveToEx(dc, cx + 16, cy - 13, nullptr);
        LineTo(dc, cx + 30, cy + 13);
        MoveToEx(dc, cx + 30, cy - 13, nullptr);
        LineTo(dc, cx + 16, cy + 13);
    } else {
        Arc(dc, cx - 8, cy - 18, cx + 34, cy + 18, cx + 13, cy - 14, cx + 13, cy + 14);
        Arc(dc, cx - 4, cy - 28, cx + 46, cy + 28, cx + 22, cy - 23, cx + 22, cy + 23);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void drawMaxRestoreIcon(HDC dc, const RECT& rect, bool maximized) {
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    ScopedGdiObject pen(CreatePen(PS_SOLID, 3, RGB(245, 248, 250)));
    const auto oldPen = SelectObject(dc, pen.object);
    const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (maximized) {
        Rectangle(dc, cx - 9, cy - 15, cx + 17, cy + 11);
        Rectangle(dc, cx - 17, cy - 7, cx + 9, cy + 19);
    } else {
        const int left = cx - 18;
        const int top = cy - 18;
        const int right = cx + 18;
        const int bottom = cy + 18;
        MoveToEx(dc, left, top + 13, nullptr);
        LineTo(dc, left, top);
        LineTo(dc, left + 13, top);
        MoveToEx(dc, right - 13, top, nullptr);
        LineTo(dc, right, top);
        LineTo(dc, right, top + 13);
        MoveToEx(dc, right, bottom - 13, nullptr);
        LineTo(dc, right, bottom);
        LineTo(dc, right - 13, bottom);
        MoveToEx(dc, left + 13, bottom, nullptr);
        LineTo(dc, left, bottom);
        LineTo(dc, left, bottom - 13);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

void drawStopIcon(HDC dc, const RECT& rect) {
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    ScopedGdiObject redBrush(CreateSolidBrush(RGB(245, 74, 74)));
    ScopedGdiObject redPen(CreatePen(PS_SOLID, 3, RGB(245, 74, 74)));
    const auto oldBrush = SelectObject(dc, redBrush.object);
    const auto oldPen = SelectObject(dc, redPen.object);
    RoundRect(dc, cx - 15, cy - 15, cx + 15, cy + 15, 5, 5);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

class OverlayControls {
public:
    bool create(HWND parent, HINSTANCE instance) {
        _parent = parent;
        const wchar_t className[] = L"AxtpMediaHostOverlayControls";
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &OverlayControls::WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = className;
        const ATOM registered = RegisterClassExW(&windowClass);
        if (registered == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        _hwnd = CreateWindowExW(0,
                                className,
                                L"",
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                0,
                                0,
                                0,
                                0,
                                parent,
                                nullptr,
                                instance,
                                this);
        return _hwnd != nullptr;
    }

    void layout(const RECT& parentClient) {
        if (_hwnd == nullptr) {
            return;
        }
        constexpr int kButtonSize = 58;
        constexpr int kButtonGap = 18;
        constexpr int kOverlayPadding = 12;
        constexpr int kRightMargin = 18;
        const int width = kButtonSize + kOverlayPadding * 2;
        const int height = kButtonSize * 3 + kButtonGap * 2 + kOverlayPadding * 2;
        const int parentWidth = parentClient.right - parentClient.left;
        const int parentHeight = parentClient.bottom - parentClient.top;
        const int x = std::max(0, parentWidth - width - kRightMargin);
        const int y = std::max(0, (parentHeight - height) / 2);
        MoveWindow(_hwnd, x, y, width, height, TRUE);
        updateOverlayRegion(_hwnd, width, height);
        SetWindowPos(_hwnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void updateMuted(bool muted) {
        _muted = muted;
        invalidate();
    }

    void updateMaximized(bool maximized) {
        _maximized = maximized;
        invalidate();
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        OverlayControls* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<OverlayControls*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<OverlayControls*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        return self->handleMessage(hwnd, message, wParam, lParam);
    }

    LRESULT handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            updateHover(point);
            if (!_trackingMouse) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                _trackingMouse = TrackMouseEvent(&track) != FALSE;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            _trackingMouse = false;
            _hoverCommand = 0;
            invalidate();
            return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(hwnd, &client);
            _pressedCommand = commandForPoint(client, point);
            if (_pressedCommand != 0) {
                SetCapture(hwnd);
                invalidate();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(hwnd, &client);
            const auto command = commandForPoint(client, point);
            const auto pressed = _pressedCommand;
            _pressedCommand = 0;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            invalidate();
            if (pressed != 0 && pressed == command && _parent != nullptr) {
                PostMessageW(_parent, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
            }
            return 0;
        }
        case WM_PAINT:
            paint(hwnd);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    void updateHover(POINT point) {
        RECT client{};
        GetClientRect(_hwnd, &client);
        const auto command = commandForPoint(client, point);
        if (command != _hoverCommand) {
            _hoverCommand = command;
            invalidate();
        }
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT paintStruct{};
        HDC dc = BeginPaint(hwnd, &paintStruct);
        RECT client{};
        GetClientRect(hwnd, &client);
        constexpr int kCommands[] = {
            kOverlayButtonMute,
            kOverlayButtonMaxRestore,
            kOverlayButtonStop,
        };
        for (const auto command : kCommands) {
            const RECT rect = buttonRectForCommand(client, command);
            const bool danger = command == kOverlayButtonStop;
            drawButtonShell(dc, rect, _hoverCommand == command, _pressedCommand == command, danger);
            switch (command) {
            case kOverlayButtonMute:
                drawVolumeIcon(dc, rect, _muted);
                break;
            case kOverlayButtonMaxRestore:
                drawMaxRestoreIcon(dc, rect, _maximized);
                break;
            case kOverlayButtonStop:
                drawStopIcon(dc, rect);
                break;
            default:
                break;
            }
        }
        EndPaint(hwnd, &paintStruct);
    }

    void invalidate() {
        if (_hwnd != nullptr) {
            InvalidateRect(_hwnd, nullptr, TRUE);
        }
    }

    HWND _parent = nullptr;
    HWND _hwnd = nullptr;
    bool _muted = false;
    bool _maximized = false;
    bool _trackingMouse = false;
    int _hoverCommand = 0;
    int _pressedCommand = 0;
};

class StatsOverlay {
public:
    bool create(HWND parent, HINSTANCE instance) {
        const wchar_t className[] = L"AxtpMediaHostStatsOverlay";
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &StatsOverlay::WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = className;
        const ATOM registered = RegisterClassExW(&windowClass);
        if (registered == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        _hwnd = CreateWindowExW(0,
                                className,
                                L"",
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                16,
                                16,
                                132,
                                42,
                                parent,
                                nullptr,
                                instance,
                                this);
        return _hwnd != nullptr;
    }

    void layout() {
        if (_hwnd != nullptr) {
            SetWindowPos(_hwnd,
                         HWND_TOP,
                         16,
                         16,
                         132,
                         42,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    void setFps(double fps, std::uint64_t frames) {
        std::ostringstream text;
        text << std::fixed << std::setprecision(1) << fps << " FPS";
        if (frames > 0) {
            text << "\nframes " << frames;
        }
        _text = text.str();
        if (_hwnd != nullptr) {
            InvalidateRect(_hwnd, nullptr, TRUE);
        }
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        StatsOverlay* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<StatsOverlay*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<StatsOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        if (message == WM_PAINT) {
            self->paint(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT paintStruct{};
        HDC dc = BeginPaint(hwnd, &paintStruct);
        RECT client{};
        GetClientRect(hwnd, &client);
        ScopedGdiObject brush(CreateSolidBrush(RGB(16, 18, 20)));
        ScopedGdiObject pen(CreatePen(PS_SOLID, 1, RGB(74, 80, 88)));
        const auto oldBrush = SelectObject(dc, brush.object);
        const auto oldPen = SelectObject(dc, pen.object);
        RoundRect(dc, client.left, client.top, client.right, client.bottom, 12, 12);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 240, 245));
        RECT textRect = client;
        textRect.left += 10;
        textRect.top += 7;
        textRect.right -= 10;
        const auto wide = widen(_text.empty() ? std::string_view("0.0 FPS") : std::string_view(_text));
        DrawTextW(dc, wide.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_NOPREFIX);
        EndPaint(hwnd, &paintStruct);
    }

    HWND _hwnd = nullptr;
    std::string _text = "0.0 FPS";
};
