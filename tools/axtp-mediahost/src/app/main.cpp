#include "profiles/media/media_host.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "transports/hidapi/hid_transport.hpp"
#include "toolkit/axtp_toolkit.hpp"
#include "media/render/win32/media_render_host.hpp"

namespace {

constexpr std::uint32_t kDefaultHidVendorId = 0x0581;
constexpr std::uint32_t kDefaultHidProductId = 0x2581;
constexpr std::uint32_t kDefaultHidUsagePage = 0x81;

volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
}

struct CliOptions {
    std::optional<std::uint32_t> vid = kDefaultHidVendorId;
    std::optional<std::uint32_t> pid = kDefaultHidProductId;
    std::optional<std::uint32_t> usagePage = kDefaultHidUsagePage;
    std::optional<std::uint32_t> usage;
    std::optional<std::uint32_t> randomSeed;
    std::string hidPath;
    std::string serial;
    bool hidTargetSpecified = false;
    std::uint32_t reportId = 0x05;
    std::uint32_t inputReportSize = 255;
    std::uint32_t outputReportSize = 255;
    std::uint32_t readBufferSize = 4096;
    std::uint32_t maxReportsPerPoll = 32;
    std::uint32_t timeoutMs = 5000;
    bool render = false;
    bool renderBackendSpecified = false;
    axtp::mediahost::RenderBackend renderBackend = axtp::mediahost::RenderBackend::None;
    axtp::mediahost::H264FeedMode h264FeedMode = axtp::mediahost::H264FeedMode::StreamChunk;
    bool noVideo = false;
    bool noAudio = false;
    bool startMuted = false;
    bool overlayEnabled = true;
    bool logBody = false;
    bool logEnabled = true;
    bool pullOnSourceEvent = false;
    axtp::mediahost::OpenMode openMode = axtp::mediahost::OpenMode::ReceiverPull;
    bool help = false;
    std::filesystem::path dumpDir;
    std::string source = "wireless_cast";
    std::string audioFormat = "adts";
    std::uint32_t audioSampleRate = 48000;
    std::uint32_t audioChannels = 1;
};

std::filesystem::path exePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (size == 0) {
        return std::filesystem::current_path() / "axtp-mediahost.exe";
    }
    buffer.resize(size);
    return std::filesystem::path(buffer);
}

std::string formatTime(const char* pattern) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &time);
    char text[64] = {};
    std::strftime(text, sizeof(text), pattern, &tm);
    return text;
}

std::wstring widen(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8,
                                             0,
                                             value.data(),
                                             static_cast<int>(value.size()),
                                             nullptr,
                                             0);
    if (required <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        0,
                        value.data(),
                        static_cast<int>(value.size()),
                        output.data(),
                        required);
    return output;
}

class LocalLogger {
public:
    LocalLogger(bool enabled, bool includeBody)
        : includeBody_(includeBody) {
        if (!enabled) {
            return;
        }
        const auto path = exePath();
        const auto dir = path.parent_path() / "axtp-mediahost-logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ostringstream name;
        name << "axtp-mediahost-" << formatTime("%Y%m%d-%H%M%S") << "-"
             << GetCurrentProcessId() << ".log";
        path_ = dir / name.str();
        stream_.open(path_, std::ios::out | std::ios::app);
        if (stream_) {
            write("log opened");
            write(std::string("exe=") + path.string());
        }
    }

    void write(std::string_view line) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << line << "\n";
        if (!stream_) {
            return;
        }
        stream_ << formatTime("%Y-%m-%d %H:%M:%S") << " " << line << "\n";
        stream_.flush();
    }

    bool includeBody() const {
        return includeBody_;
    }

private:
    bool includeBody_ = false;
    std::mutex mutex_;
    std::filesystem::path path_;
    std::ofstream stream_;
};

class StatusWindow {
public:
    ~StatusWindow() {
        stop();
    }

    void start() {
        running_.store(true);
        thread_ = std::thread([this]() { run(); });
    }

    void stop() {
        running_.store(false);
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hwnd = hwnd_;
        }
        if (hwnd != nullptr) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void setText(std::string value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            text_ = std::move(value);
            if (hwnd_ != nullptr) {
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
        }
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        StatusWindow* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<StatusWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<StatusWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        switch (message) {
        case WM_PAINT:
            self->paint(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->hwnd_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    void run() {
        const wchar_t className[] = L"AxtpMediaHostStatusWindow";
        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = &StatusWindow::WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = className;
        RegisterClassW(&windowClass);

        HWND hwnd = CreateWindowExW(0,
                                    className,
                                    L"AXTP MediaHost",
                                    WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    720,
                                    360,
                                    nullptr,
                                    nullptr,
                                    GetModuleHandleW(nullptr),
                                    this);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hwnd_ = hwnd;
        }
        if (hwnd == nullptr) {
            return;
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        MSG message = {};
        while (running_.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT paintStruct = {};
        HDC dc = BeginPaint(hwnd, &paintStruct);
        RECT client = {};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(30, 34, 38));

        std::string text;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            text = text_;
        }
        if (text.empty()) {
            text = "AXTP MediaHost waiting for streams...";
        }
        auto wide = widen(text);
        RECT textRect = client;
        textRect.left += 24;
        textRect.top += 24;
        textRect.right -= 24;
        textRect.bottom -= 24;
        DrawTextW(dc, wide.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
        EndPaint(hwnd, &paintStruct);
    }

    std::atomic_bool running_{false};
    std::thread thread_;
    std::mutex mutex_;
    HWND hwnd_ = nullptr;
    std::string text_;
};

void printUsage() {
    std::cout
        << "Usage: axtp-mediahost --path <hid path> [options]\n"
        << "       axtp-mediahost [--vid 0x0581 --pid 0x2581 --usage-page 0x81] [options]\n"
        << "\n"
        << "HID options:\n"
        << "  --path, --hid-path <path> Open HIDAPI path with hid_open_path\n"
        << "  --vid <hex|dec>          HID vendor id, default 0x0581\n"
        << "  --pid <hex|dec>          HID product id, default 0x2581\n"
        << "  --usage-page <hex|dec>   HID usage page filter, default 0x81\n"
        << "  --usage <hex|dec>        HID usage filter\n"
        << "  --serial <value>         HID serial value for VID/PID open\n"
        << "  --report-id <id>         HID report id, default 0x05\n"
        << "  --input-report-size <n>  HID input report bytes incl report id, default 255\n"
        << "  --output-report-size <n> HID output report bytes incl report id, default 255\n"
        << "  --read-buffer-size <n>   HID read buffer bytes, default 4096\n"
        << "  --timeout <ms>           App-ready timeout, default 5000\n"
        << "\n"
        << "Media options:\n"
        << "  --render                 Show a Windows video window and play received streams\n"
        << "  --render-backend <name>  none, self-test, or mf-d3d11; default mf-d3d11 with --render\n"
        << "  --h264-feed-mode <mode>  stream-chunk (default) or annexb-au\n"
        << "  --dump-dir <dir>         Dump received video/audio bytes to .h264/.aac files\n"
        << "  --no-video               Reject video.openStream\n"
        << "  --no-audio               Reject audio.openStream\n"
        << "  --start-muted            Start audio renderer muted\n"
        << "  --no-overlay             Disable FPS overlay and floating window controls\n"
        << "  --open-mode <mode>       receiver-pull (default), producer-open, or both\n"
        << "  --source <source>        Default source, default wireless_cast\n"
        << "  --audio-format <fmt>     AAC format accepted by MVP, default adts\n"
        << "  --audio-sample-rate <n>  AAC sample rate hint/override, default 48000\n"
        << "  --audio-channels <n>     AAC channel hint/override, default 1\n"
        << "  --pull-on-source-event   Deprecated alias for --open-mode receiver-pull\n"
        << "\n"
        << "Logging options:\n"
        << "  --log                    Write log beside exe under axtp-mediahost-logs (default)\n"
        << "  --no-log                 Disable file log\n"
        << "  --log-body               Log JSON request/response bodies\n"
        << "  --random-seed <hex|dec>  Override Identify randomSeed\n"
        << "  -h, --help               Show this help\n";
}

std::optional<std::uint32_t> parseU32(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(text, &consumed, 0);
        if (consumed != text.size() ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<axtp::mediahost::OpenMode> parseOpenMode(const std::string& text) {
    if (text == "receiver-pull") {
        return axtp::mediahost::OpenMode::ReceiverPull;
    }
    if (text == "producer-open") {
        return axtp::mediahost::OpenMode::ProducerOpen;
    }
    if (text == "both") {
        return axtp::mediahost::OpenMode::Both;
    }
    return std::nullopt;
}

bool parseOptions(int argc, char** argv, CliOptions* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        auto parseOptionU32 = [&](const char* name, std::uint32_t* output) {
            const auto* value = requireValue(name);
            if (value == nullptr) {
                return false;
            }
            const auto parsed = parseU32(value);
            if (!parsed.has_value()) {
                std::cerr << "invalid " << name << "\n";
                return false;
            }
            *output = *parsed;
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            options->help = true;
            return true;
        }
        if (arg == "--path" || arg == "--hid-path") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            options->hidPath = value;
            options->hidTargetSpecified = true;
            continue;
        }
        if (arg == "--serial") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            options->serial = value;
            options->hidTargetSpecified = true;
            continue;
        }
        if (arg == "--vid" || arg == "--pid" || arg == "--usage-page" ||
            arg == "--usagepage" || arg == "--hid-usage-page" ||
            arg == "--hid-usagepage" || arg == "--usage" ||
            arg == "--hid-usage" || arg == "--random-seed") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            const auto parsed = parseU32(value);
            if (!parsed.has_value()) {
                std::cerr << "invalid " << arg << "\n";
                return false;
            }
            if (arg == "--vid") {
                if (*parsed > 0xFFFF) {
                    std::cerr << "invalid " << arg << "\n";
                    return false;
                }
                options->vid = *parsed;
                options->hidTargetSpecified = true;
            } else if (arg == "--pid") {
                if (*parsed > 0xFFFF) {
                    std::cerr << "invalid " << arg << "\n";
                    return false;
                }
                options->pid = *parsed;
                options->hidTargetSpecified = true;
            } else if (arg == "--usage-page" || arg == "--usagepage" ||
                       arg == "--hid-usage-page" || arg == "--hid-usagepage") {
                if (*parsed > 0xFFFF) {
                    std::cerr << "invalid " << arg << "\n";
                    return false;
                }
                options->usagePage = *parsed;
                options->hidTargetSpecified = true;
            } else if (arg == "--usage" || arg == "--hid-usage") {
                if (*parsed > 0xFFFF) {
                    std::cerr << "invalid " << arg << "\n";
                    return false;
                }
                options->usage = *parsed;
                options->hidTargetSpecified = true;
            } else {
                options->randomSeed = *parsed;
            }
            continue;
        }
        if (arg == "--report-id") {
            if (!parseOptionU32(arg.c_str(), &options->reportId) || options->reportId > 0xFF) {
                return false;
            }
            continue;
        }
        if (arg == "--input-report-size") {
            if (!parseOptionU32(arg.c_str(), &options->inputReportSize)) {
                return false;
            }
            continue;
        }
        if (arg == "--output-report-size") {
            if (!parseOptionU32(arg.c_str(), &options->outputReportSize)) {
                return false;
            }
            continue;
        }
        if (arg == "--read-buffer-size") {
            if (!parseOptionU32(arg.c_str(), &options->readBufferSize)) {
                return false;
            }
            continue;
        }
        if (arg == "--max-reports-per-poll") {
            if (!parseOptionU32(arg.c_str(), &options->maxReportsPerPoll)) {
                return false;
            }
            continue;
        }
        if (arg == "--timeout") {
            if (!parseOptionU32(arg.c_str(), &options->timeoutMs)) {
                return false;
            }
            continue;
        }
        if (arg == "--render") {
            options->render = true;
            continue;
        }
        if (arg == "--render-backend") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            bool ok = false;
            const auto backend = axtp::mediahost::parseRenderBackendOrNone(value, &ok);
            if (!ok) {
                std::cerr << "invalid --render-backend, expected none, self-test, or "
                             "mf-d3d11\n";
                return false;
            }
            options->renderBackend = backend;
            options->renderBackendSpecified = true;
            options->render = backend != axtp::mediahost::RenderBackend::None;
            continue;
        }
        if (arg == "--h264-feed-mode") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            bool ok = false;
            const auto mode = axtp::mediahost::parseH264FeedModeOrDefault(value, &ok);
            if (!ok) {
                std::cerr << "invalid --h264-feed-mode, expected stream-chunk or annexb-au\n";
                return false;
            }
            options->h264FeedMode = mode;
            continue;
        }
        if (arg == "--no-video") {
            options->noVideo = true;
            continue;
        }
        if (arg == "--no-audio") {
            options->noAudio = true;
            continue;
        }
        if (arg == "--start-muted") {
            options->startMuted = true;
            continue;
        }
        if (arg == "--no-overlay") {
            options->overlayEnabled = false;
            continue;
        }
        if (arg == "--dump-dir") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            options->dumpDir = value;
            continue;
        }
        if (arg == "--source") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            options->source = value;
            continue;
        }
        if (arg == "--audio-format") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            options->audioFormat = value;
            continue;
        }
        if (arg == "--audio-sample-rate") {
            if (!parseOptionU32(arg.c_str(), &options->audioSampleRate) ||
                options->audioSampleRate == 0) {
                return false;
            }
            continue;
        }
        if (arg == "--audio-channels") {
            if (!parseOptionU32(arg.c_str(), &options->audioChannels) ||
                options->audioChannels == 0 || options->audioChannels > 8) {
                return false;
            }
            continue;
        }
        if (arg == "--open-mode") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            const auto parsed = parseOpenMode(value);
            if (!parsed.has_value()) {
                std::cerr << "invalid --open-mode, expected receiver-pull, producer-open, or both\n";
                return false;
            }
            options->openMode = *parsed;
            continue;
        }
        if (arg == "--pull-on-source-event") {
            options->pullOnSourceEvent = true;
            options->openMode = axtp::mediahost::OpenMode::ReceiverPull;
            continue;
        }
        if (arg == "--log") {
            options->logEnabled = true;
            continue;
        }
        if (arg == "--no-log") {
            options->logEnabled = false;
            continue;
        }
        if (arg == "--log-body") {
            options->logBody = true;
            continue;
        }

        std::cerr << "unknown option: " << arg << "\n";
        return false;
    }
    return true;
}

bool hasHidTarget(const CliOptions& options) {
    return !options.hidPath.empty() || (options.vid.has_value() && options.pid.has_value());
}

bool hasExplicitHidTarget(const CliOptions& options) {
    return options.hidTargetSpecified;
}

const char* errorName(axtp::ErrorCode code) {
    return axtp::toolkit::errorName(code);
}

}  // namespace

int main(int argc, char** argv) {
    CliOptions options;
    if (!parseOptions(argc, argv, &options)) {
        return 2;
    }
    if (options.help) {
        printUsage();
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    LocalLogger logger(options.logEnabled, options.logBody);
    logger.write("AXTP MediaHost starting");
    if (options.pullOnSourceEvent) {
        logger.write("--pull-on-source-event is deprecated; using --open-mode receiver-pull");
    }

    const auto effectiveRenderBackend =
        options.render ? (options.renderBackendSpecified ? options.renderBackend
                                                         : axtp::mediahost::RenderBackend::MfD3d11)
                       : axtp::mediahost::RenderBackend::None;
    axtp::mediahost::MediaRenderHost renderer(
        [&logger](std::string_view line) { logger.write(line); });
    if (options.renderBackendSpecified &&
        effectiveRenderBackend == axtp::mediahost::RenderBackend::SelfTest &&
        !hasExplicitHidTarget(options)) {
        if (options.noVideo) {
            std::cerr << "--render-backend self-test requires video; remove --no-video\n";
            return 2;
        }
        axtp::mediahost::MediaRenderHostOptions renderOptions;
        renderOptions.backend = effectiveRenderBackend;
        renderOptions.h264FeedMode = options.h264FeedMode;
        renderOptions.enableVideo = !options.noVideo;
        renderOptions.enableAudio = false;
        renderOptions.startMuted = options.startMuted;
        renderOptions.overlayEnabled = options.overlayEnabled;
        if (!renderer.start(renderOptions)) {
            return 5;
        }
        logger.write("renderer self-test running without HID target");
        while (g_running != 0 && renderer.running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        renderer.stop();
        return 0;
    }
    if (!hasHidTarget(options)) {
        std::cerr << "axtp-mediahost requires --path/--hid-path or both --vid and --pid\n";
        return 2;
    }

    axtp::mediahost::MediaHostOptions mediaOptions;
    mediaOptions.acceptVideo = !options.noVideo;
    mediaOptions.acceptAudio = !options.noAudio;
    mediaOptions.logBody = options.logBody;
    mediaOptions.openMode = options.openMode;
    mediaOptions.dumpDir = options.dumpDir;
    mediaOptions.source = options.source;
    mediaOptions.audioFormat = options.audioFormat;
    mediaOptions.audioSampleRate = options.audioSampleRate;
    mediaOptions.audioChannels = options.audioChannels;
    mediaOptions.streamSink =
        effectiveRenderBackend == axtp::mediahost::RenderBackend::None ? nullptr : &renderer;

    if (!mediaOptions.dumpDir.empty() && mediaOptions.dumpDir.is_relative()) {
        mediaOptions.dumpDir = exePath().parent_path() / mediaOptions.dumpDir;
    }

    axtp::BasicBroker<> broker;
    axtp::mediahost::MediaStreamRegistry registry(
        mediaOptions, [&logger](std::string_view line) { logger.write(line); });
    axtp::mediahost::MediaPullCoordinator pullCoordinator(
        registry,
        "",
        std::chrono::milliseconds(options.timeoutMs),
        [&logger](std::string_view line) { logger.write(line); });
    axtp::mediahost::MediaCloseCoordinator closeCoordinator(
        "",
        std::chrono::milliseconds(options.timeoutMs),
        [&logger](std::string_view line) { logger.write(line); });
    axtp::mediahost::installMediaHostHandlers(broker, registry);
    broker.registerEventHandler([&logger, &options, &pullCoordinator](
                                    const axtp::BrokerContext&,
                                    const axtp::RpcPayload& event) {
        std::ostringstream out;
        out << "event id=0x" << std::hex << std::uppercase << event.methodOrEventId;
        if (!event.meta.jsonMethodOrEventName.empty()) {
            out << " name=" << event.meta.jsonMethodOrEventName;
        }
        if (options.logBody && !event.body.empty()) {
            out << " body=" << std::string(event.body.begin(), event.body.end());
        }
        logger.write(out.str());
        pullCoordinator.handleEvent(event);
    });

    std::ostringstream modeLog;
    modeLog << "MediaHost openMode=" << axtp::mediahost::openModeName(options.openMode)
            << " video=" << (mediaOptions.acceptVideo ? "enabled" : "disabled")
            << " audio=" << (mediaOptions.acceptAudio ? "enabled" : "disabled")
            << " renderBackend=" << axtp::mediahost::renderBackendName(effectiveRenderBackend)
            << " h264FeedMode=" << axtp::mediahost::h264FeedModeName(options.h264FeedMode)
            << " overlay=" << (options.overlayEnabled ? "enabled" : "disabled")
            << " startMuted=" << (options.startMuted ? "true" : "false")
            << " audioSampleRate=" << mediaOptions.audioSampleRate
            << " audioChannels=" << mediaOptions.audioChannels;
    if (axtp::mediahost::receiverPullEnabled(options.openMode)) {
        modeLog << " receiver-pull=wait source-state events then send openStream";
    }
    if (axtp::mediahost::producerOpenEnabled(options.openMode)) {
        modeLog << " producer-open=accept device-initiated openStream";
    }
    logger.write(modeLog.str());

    axtp::toolkit::HidOpenOptions hidOpenOptions;
    hidOpenOptions.vendorId = options.vid;
    hidOpenOptions.productId = options.pid;
    hidOpenOptions.usagePage = options.usagePage;
    hidOpenOptions.usage = options.usage;
    hidOpenOptions.devicePath = options.hidPath;
    hidOpenOptions.serialNumber = options.serial;
    hidOpenOptions.reportId = options.reportId;
    hidOpenOptions.inputReportSize = options.inputReportSize;
    hidOpenOptions.outputReportSize = options.outputReportSize;
    hidOpenOptions.readBufferSize = options.readBufferSize;
    hidOpenOptions.maxReportsPerPoll = options.maxReportsPerPoll;
    hidOpenOptions.useReadThread = true;
    hidOpenOptions.readThreadTimeoutMs = 1000;
    auto hidOptions = axtp::toolkit::makeHidTransportOptions(hidOpenOptions);

    std::ostringstream openLog;
    openLog << "opening HID"
            << " path=" << (options.hidPath.empty() ? "<none>" : options.hidPath)
            << " vid=" << axtp::mediahost::toHexU32(options.vid.value_or(0))
            << " pid=" << axtp::mediahost::toHexU32(options.pid.value_or(0))
            << " usagePage=" << axtp::mediahost::toHexU32(options.usagePage.value_or(0))
            << " usage=" << axtp::mediahost::toHexU32(options.usage.value_or(0))
            << " reportId=0x" << std::hex << std::uppercase << options.reportId
            << std::dec
            << " inputReportSize=" << options.inputReportSize
            << " outputReportSize=" << options.outputReportSize
            << " readBufferSize=" << options.readBufferSize;
    logger.write(openLog.str());

    auto transport = std::make_unique<axtp::HidTransport>(hidOptions);
    auto* hidTransport = transport.get();
    axtp::AxtpEndpoint<axtp::BasicBroker<>> endpoint(broker);
    endpoint.attachTransport(*transport);
    transport->open();
    if (!hidTransport->isOpen()) {
        logger.write("failed to open HID device");
        return 4;
    }
    logger.write("HID device opened");

    if (effectiveRenderBackend != axtp::mediahost::RenderBackend::None) {
        axtp::mediahost::MediaRenderHostOptions renderOptions;
        renderOptions.backend = effectiveRenderBackend;
        renderOptions.h264FeedMode = options.h264FeedMode;
        renderOptions.enableVideo = !options.noVideo;
        renderOptions.enableAudio = !options.noAudio;
        renderOptions.startMuted = options.startMuted;
        renderOptions.overlayEnabled = options.overlayEnabled;
        if (!renderer.start(renderOptions)) {
            transport->close();
            return 5;
        }
        renderer.setStatusText("AXTP MediaHost connected. Waiting for app-ready...");
    }

    const auto started = std::chrono::steady_clock::now();
    axtp::toolkit::EndpointAppReadyOptions appReadyOptions;
    appReadyOptions.timeout = std::chrono::milliseconds(options.timeoutMs);
    appReadyOptions.randomSeed = options.randomSeed;
    appReadyOptions.includeBody = options.logBody;
    appReadyOptions.log = [&logger](std::string_view line) { logger.write(line); };
    const auto ready = axtp::toolkit::ensureEndpointAppReady(endpoint, *transport, appReadyOptions);
    const auto readyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    if (!ready.ok) {
        logger.write("APP_READY failed stage=" + ready.stage + " status=" +
                     errorName(ready.status));
        transport->close();
        renderer.stop();
        return 4;
    }
    logger.write("APP_READY ok sid=" + ready.sid + " randomSeed=" +
                 axtp::mediahost::toHexU32(ready.randomSeed) +
                 " elapsedMs=" + std::to_string(readyMs));
    pullCoordinator.setSid(ready.sid);
    closeCoordinator.setSid(ready.sid);

    if (axtp::mediahost::receiverPullEnabled(options.openMode) &&
        axtp::mediahost::producerOpenEnabled(options.openMode)) {
        logger.write("MediaHost ready; waiting for source events and device openStream");
    } else if (axtp::mediahost::receiverPullEnabled(options.openMode)) {
        logger.write("MediaHost ready; waiting for audio/video source events to pull streams");
    } else {
        logger.write("MediaHost ready; waiting for device-initiated audio/video.openStream");
    }
    auto nextUiUpdate = std::chrono::steady_clock::now();
    while (g_running != 0) {
        endpoint.poll(64);
        pullCoordinator.poll(endpoint);
        closeCoordinator.poll(endpoint);
        if (effectiveRenderBackend != axtp::mediahost::RenderBackend::None &&
            !renderer.running()) {
            logger.write("renderer window closed; exiting MediaHost");
            break;
        }
        if (effectiveRenderBackend != axtp::mediahost::RenderBackend::None &&
            renderer.consumeStopCastingRequested()) {
            const auto activeStreams = registry.activeStreamsSnapshot();
            if (activeStreams.empty()) {
                logger.write("stop casting requested: no active streams to close");
            } else {
                logger.write("stop casting requested: closing " +
                             std::to_string(activeStreams.size()) +
                             " active stream(s), keeping AXTP session open");
            }
            for (const auto& stream : activeStreams) {
                closeCoordinator.sendClose(endpoint, stream);
                const auto result = registry.closeLocal(stream.kind, stream.streamId);
                if (result.status != axtp::ErrorCode::Success) {
                    logger.write("local close failed streamId=" +
                                 axtp::mediahost::toHexU32(stream.streamId) +
                                 " status=" + errorName(result.status));
                }
            }
            pullCoordinator.clearAll("stop casting");
            renderer.setStatusText("Casting stopped. AXTP session is still connected.");
        }
        if (effectiveRenderBackend != axtp::mediahost::RenderBackend::None &&
            std::chrono::steady_clock::now() >= nextUiUpdate) {
            const auto stats = registry.stats();
            std::ostringstream status;
            status << "AXTP MediaHost\n"
                   << "sid: " << ready.sid << "\n"
                   << "open mode: " << axtp::mediahost::openModeName(options.openMode) << "\n"
                   << "active streams: " << registry.activeStreamCount() << "\n"
                   << "pending pulls: " << pullCoordinator.pendingCount() << "\n"
                   << "pending closes: " << closeCoordinator.pendingCount() << "\n"
                   << "muted: " << (renderer.isMuted() ? "true" : "false") << "\n"
                   << "video: " << stats.videoChunks << " chunks, " << stats.videoBytes
                   << " bytes\n"
                   << "audio: " << stats.audioChunks << " chunks, " << stats.audioBytes
                   << " bytes\n"
                   << "unknown: " << stats.unknownChunks << " seqGaps: " << stats.seqGaps
                   << " duplicateSeq: " << stats.duplicateSeq << "\n"
                   << "render backend: "
                   << axtp::mediahost::renderBackendName(effectiveRenderBackend);
            renderer.setStatusText(status.str());
            nextUiUpdate = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto stats = registry.stats();
    logger.write("MediaHost stopping videoChunks=" + std::to_string(stats.videoChunks) +
                 " videoBytes=" + std::to_string(stats.videoBytes) +
                 " audioChunks=" + std::to_string(stats.audioChunks) +
                 " audioBytes=" + std::to_string(stats.audioBytes) + " unknownChunks=" +
                 std::to_string(stats.unknownChunks) + " seqGaps=" + std::to_string(stats.seqGaps));
    transport->close();
    renderer.stop();
    return 0;
}
