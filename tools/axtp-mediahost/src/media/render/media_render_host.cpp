#include "media/render/media_render_host.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>

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
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
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

constexpr int kOverlayButtonMute = 1001;
constexpr int kOverlayButtonStop = 1002;
constexpr int kOverlayButtonMaxRestore = 1003;
constexpr UINT kUpdateMutedMessage = WM_APP + 41;
constexpr UINT_PTR kSelfTestTimerId = 1;
constexpr UINT_PTR kFpsTimerId = 2;

struct ScopedGdiObject {
    explicit ScopedGdiObject(HGDIOBJ value) : object(value) {}
    ~ScopedGdiObject() {
        if (object != nullptr) {
            DeleteObject(object);
        }
    }
    HGDIOBJ object = nullptr;
};

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

} // namespace
} // namespace axtp::mediahost

namespace axtp::mediahost {

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
            {
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
            processH264Sample(stream.data, unit, stream.cursor, "stream-chunk");
            return;
        }

        auto units = _assembler.pushChunk(stream.data);
        {
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
            if (_presenter.present(sample)) {
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
        if (screenPoint.y < window.top + 44) {
            return HTCAPTION;
        }
        return HTCLIENT;
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
