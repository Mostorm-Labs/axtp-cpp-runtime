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
