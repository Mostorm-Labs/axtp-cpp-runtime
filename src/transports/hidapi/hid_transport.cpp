#include "transports/hidapi/hid_transport.hpp"

#include <algorithm>
#include <cwchar>
#include <hidapi.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <hidapi_winapi.h>
#include <windows.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#endif
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace axtp {
namespace {

bool isHighSurrogate(std::uint32_t codePoint) {
    return codePoint >= 0xD800 && codePoint <= 0xDBFF;
}

bool isLowSurrogate(std::uint32_t codePoint) {
    return codePoint >= 0xDC00 && codePoint <= 0xDFFF;
}

void appendUtf8Replacement(std::string& output) {
    output.append("\xEF\xBF\xBD");
}

void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (isHighSurrogate(codePoint) || isLowSurrogate(codePoint)) {
        appendUtf8Replacement(output);
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0x10FFFF) {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        appendUtf8Replacement(output);
    }
}

std::string wideToUtf8(const wchar_t* value) {
    if (value == nullptr || value[0] == L'\0') {
        return {};
    }

    std::string output;
    for (const wchar_t* cursor = value; *cursor != L'\0'; ++cursor) {
        auto codePoint = static_cast<std::uint32_t>(*cursor);
        if constexpr (sizeof(wchar_t) == 2) {
            if (isHighSurrogate(codePoint)) {
                const auto low = static_cast<std::uint32_t>(*(cursor + 1));
                if (isLowSurrogate(low)) {
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    ++cursor;
                } else {
                    appendUtf8Replacement(output);
                    continue;
                }
            } else if (isLowSurrogate(codePoint)) {
                appendUtf8Replacement(output);
                continue;
            }
        }
        appendUtf8(output, codePoint);
    }
    return output;
}

std::string busTypeName(hid_bus_type busType) {
    switch (busType) {
    case HID_API_BUS_USB:
        return "usb";
    case HID_API_BUS_BLUETOOTH:
        return "bluetooth";
    case HID_API_BUS_I2C:
        return "i2c";
    case HID_API_BUS_SPI:
        return "spi";
    case HID_API_BUS_VIRTUAL:
        return "virtual";
    case HID_API_BUS_UNKNOWN:
    default:
        return "unknown";
    }
}

bool hasUsageFilter(const HidTransportOptions& options) {
    return options.usagePage != 0 || options.usage != 0;
}

bool matchesDeviceFilter(const HidDeviceInfo& device, const HidTransportOptions& options) {
    if (!options.serialNumber.empty() && device.serialNumber != options.serialNumber) {
        return false;
    }
    if (options.usagePage != 0 && device.usagePage != options.usagePage) {
        return false;
    }
    if (options.usage != 0 && device.usage != options.usage) {
        return false;
    }
    return !device.path.empty();
}

std::string usageFilterDescription(const HidTransportOptions& options) {
    std::ostringstream out;
    out << "vid=0x" << std::hex << std::uppercase << options.vendorId
        << " pid=0x" << options.productId;
    if (options.usagePage != 0) {
        out << " usagePage=0x" << options.usagePage;
    }
    if (options.usage != 0) {
        out << " usage=0x" << options.usage;
    }
    if (!options.serialNumber.empty()) {
        out << " serial=" << options.serialNumber;
    }
    return out.str();
}

std::optional<std::string> resolveHidPathFromFilters(const HidTransportOptions& options) {
    const auto devices = enumerateHidDevices(options.vendorId, options.productId);
    for (const auto& device : devices) {
        if (matchesDeviceFilter(device, options)) {
            return device.path;
        }
    }
    return std::nullopt;
}

#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const auto required = MultiByteToWideChar(CP_UTF8,
                                              MB_ERR_INVALID_CHARS,
                                              value.c_str(),
                                              -1,
                                              nullptr,
                                              0);
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    const auto written = MultiByteToWideChar(CP_UTF8,
                                             MB_ERR_INVALID_CHARS,
                                             value.c_str(),
                                             -1,
                                             output.data(),
                                             required);
    if (written <= 0) {
        return {};
    }
    if (!output.empty() && output.back() == L'\0') {
        output.pop_back();
    }
    return output;
}

HidReportLengths readWindowsReportLengths(const std::string& path) {
    HidReportLengths lengths;
    const auto widePath = utf8ToWide(path);
    if (widePath.empty()) {
        return lengths;
    }

    HANDLE handle = CreateFileW(widePath.c_str(),
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return lengths;
    }

    PHIDP_PREPARSED_DATA preparsedData = nullptr;
    if (HidD_GetPreparsedData(handle, &preparsedData) != FALSE && preparsedData != nullptr) {
        HIDP_CAPS caps{};
        if (HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS) {
            lengths.inputReportSize = caps.InputReportByteLength;
            lengths.outputReportSize = caps.OutputReportByteLength;
            lengths.featureReportSize = caps.FeatureReportByteLength;
        }
        HidD_FreePreparsedData(preparsedData);
    }
    CloseHandle(handle);
    return lengths;
}
#endif

class HidApiBackend : public IHidBackend {
public:
    ~HidApiBackend() override {
        close();
    }

    bool open(const HidTransportOptions& options) override {
        close();

        std::string pathToOpen = options.devicePath;
        if (pathToOpen.empty() && hasUsageFilter(options)) {
            const auto resolvedPath = resolveHidPathFromFilters(options);
            if (!resolvedPath.has_value()) {
                setLastError("no HID device matched " + usageFilterDescription(options));
                return false;
            }
            pathToOpen = *resolvedPath;
        }

        if (hid_init() != 0) {
            setLastError("hid_init failed");
            return false;
        }
        _initialized = true;

        if (!pathToOpen.empty()) {
            _handle = hid_open_path(pathToOpen.c_str());
        } else {
            std::wstring serial;
            const wchar_t* serialPtr = nullptr;
            if (!options.serialNumber.empty()) {
                serial.assign(options.serialNumber.begin(), options.serialNumber.end());
                serialPtr = serial.c_str();
            }
            _handle = hid_open(options.vendorId, options.productId, serialPtr);
        }
        if (_handle == nullptr) {
            if (!pathToOpen.empty()) {
                setLastError("hid_open_path failed for path=" + pathToOpen);
            } else {
                setLastError("hid_open failed for " + usageFilterDescription(options));
            }
            close();
            return false;
        }
        _reportId = options.reportId;
#if defined(_WIN32)
        _reportLengths = pathToOpen.empty() ? HidReportLengths{} : readWindowsReportLengths(pathToOpen);
#endif
        hid_set_nonblocking(_handle, 0);
#if defined(_WIN32)
        hid_winapi_set_write_timeout(_handle, options.writeTimeoutMs);
#endif
        setLastError({});
        return true;
    }

    void close() override {
        if (_handle != nullptr) {
            hid_close(_handle);
            _handle = nullptr;
        }
#if defined(_WIN32)
        _reportLengths = {};
#endif
        if (_initialized) {
            hid_exit();
            _initialized = false;
        }
    }

    bool writeReport(const Byte* data, std::size_t size) override {
        if (_handle == nullptr || data == nullptr || size == 0) {
            setLastError("invalid HID write arguments");
            return false;
        }
        const auto written = hid_write(_handle, data, size);
        if (written < static_cast<int>(size)) {
            setLastError(wideToUtf8(hid_error(_handle)));
            return false;
        }
        setLastError({});
        return true;
    }

    std::optional<std::size_t>
    readReport(Byte* data, std::size_t size, std::uint32_t timeoutMs) override {
        if (_handle == nullptr || data == nullptr || size == 0) {
            setLastError("invalid HID read arguments");
            return std::nullopt;
        }
        const auto timeout = timeoutMs > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
                                 ? std::numeric_limits<int>::max()
                                 : static_cast<int>(timeoutMs);
        const auto read = hid_read_timeout(_handle, data, size, timeout);
        if (read < 0) {
            setLastError(wideToUtf8(hid_read_error(_handle)));
            return std::nullopt;
        }
        setLastError({});
#if defined(_WIN32) || defined(__APPLE__)
        if (_reportId == 0 && read > 0) {
            const auto readSize = static_cast<std::size_t>(read);
            if (readSize < size) {
                std::move_backward(data, data + readSize, data + readSize + 1);
                data[0] = 0;
                return readSize + 1;
            }
        }
#endif
        return static_cast<std::size_t>(read);
    }

    std::string lastError() const override {
        std::lock_guard<std::mutex> lock(_lastErrorMutex);
        return _lastError;
    }

    HidReportLengths reportLengths() const override {
        return _reportLengths;
    }

private:
    void setLastError(std::string message) {
        std::lock_guard<std::mutex> lock(_lastErrorMutex);
        _lastError = std::move(message);
    }

    hid_device* _handle = nullptr;
    bool _initialized = false;
    std::uint8_t _reportId = 0;
    HidReportLengths _reportLengths;
    mutable std::mutex _lastErrorMutex;
    std::string _lastError;
};

}  // namespace

std::vector<HidDeviceInfo> enumerateHidDevices(std::uint16_t vendorId, std::uint16_t productId) {
    std::vector<HidDeviceInfo> devices;
    if (hid_init() != 0) {
        return devices;
    }

    auto* list = hid_enumerate(vendorId, productId);
    for (auto* item = list; item != nullptr; item = item->next) {
        HidDeviceInfo info;
        info.path = item->path != nullptr ? item->path : "";
        info.vendorId = item->vendor_id;
        info.productId = item->product_id;
        info.releaseNumber = item->release_number;
        info.serialNumber = wideToUtf8(item->serial_number);
        info.manufacturer = wideToUtf8(item->manufacturer_string);
        info.product = wideToUtf8(item->product_string);
        info.usagePage = item->usage_page;
        info.usage = item->usage;
        info.interfaceNumber = item->interface_number;
        info.busType = busTypeName(item->bus_type);
        devices.push_back(std::move(info));
    }
    hid_free_enumeration(list);
    hid_exit();
    return devices;
}

HidTransport::HidTransport(HidTransportOptions options)
    : _options(std::move(options)) {}

HidTransport::HidTransport(HidTransportOptions options, std::unique_ptr<IHidBackend> backend)
    : _options(std::move(options))
    , _backend(std::move(backend)) {}

HidTransport::~HidTransport() {
    close();
}

void HidTransport::bind(IByteSink& sink) {
    _sink = &sink;
}

void HidTransport::open() {
    if (_open) {
        return;
    }
    if (_backend == nullptr) {
        _backend = makeDefaultBackend();
    }
    const bool opened = _backend != nullptr && _backend->open(_options);
    _open.store(opened);
    if (opened) {
        const auto lengths = _backend->reportLengths();
        if (_options.inputReportSize == 0 && lengths.inputReportSize > 0) {
            _options.inputReportSize = lengths.inputReportSize;
        }
        if (_options.outputReportSize == 0 && lengths.outputReportSize > 0) {
            _options.outputReportSize = lengths.outputReportSize;
        }
        if (_options.readBufferSize < _options.inputReportSize) {
            _options.readBufferSize = _options.inputReportSize;
        }
        startReadThread();
    }
}

void HidTransport::close() {
    stopReadThread();
    if (_backend != nullptr) {
        _backend->close();
    }
    _open.store(false);
}

void HidTransport::poll() {
    if (!_open || _backend == nullptr || _sink == nullptr || _options.inputReportSize <= 1) {
        return;
    }

    if (_options.useReadThread) {
        drainQueuedReports();
        return;
    }

    Bytes report(inputReadBufferSize(), 0);
    for (std::size_t index = 0; index < _options.maxReportsPerPoll; ++index) {
        std::fill(report.begin(), report.end(), 0);
        const auto read = _backend->readReport(report.data(), report.size(), 0);
        if (!read.has_value() || *read == 0) {
            if (!read.has_value()) {
                ++_readErrors;
                traceReport(HidReportTraceKind::ReadError, nullptr, 0, 0, _backend->lastError());
            } else {
                traceReport(HidReportTraceKind::ReadTimeout, nullptr, 0, 0);
            }
            return;
        }
        const auto readSize = std::min(*read, report.size());
        traceReport(HidReportTraceKind::ReadReport, report.data(), readSize, 0);
        handleReadReport(report.data(), readSize, false);
    }
}

void HidTransport::sendBytes(const Byte* data, std::size_t size) {
    if (!_open || _backend == nullptr || data == nullptr || size == 0) {
        return;
    }
    const auto capacity = outputPayloadSize();
    if (capacity == 0) {
        return;
    }

    traceReport(HidReportTraceKind::WriteFrame, data, size, 0);

    Bytes report(_options.outputReportSize, 0);
    for (std::size_t offset = 0; offset < size;) {
        std::fill(report.begin(), report.end(), 0);
        report[0] = _options.reportId;
        const auto chunkSize = std::min(capacity, size - offset);
        std::copy(data + offset, data + offset + chunkSize, report.begin() + 1);
        if (!_backend->writeReport(report.data(), report.size())) {
            ++_writeErrors;
            traceReport(
                HidReportTraceKind::WriteError, report.data(), report.size(), 0, _backend->lastError());
            return;
        }
        ++_writeReports;
        _writeBytes.fetch_add(report.size());
        traceReport(HidReportTraceKind::WriteReport, report.data(), report.size(), 0);
        offset += chunkSize;
    }
}

TransportProfile HidTransport::profile() const {
    TransportProfile profile;
    profile.kind = TransportKind::Hid;
    profile.wireMode = AxtpWireMode::FramedBinary;
    profile.defaultRpcEncoding = jsonBinaryRpcEncoding();
    profile.messageOriented = true;
    profile.supportsTextMessage = false;
    profile.supportsBinaryMessage = true;
    profile.preferredFrameSize = outputPayloadSize();
    return profile;
}

bool HidTransport::isOpen() const {
    return _open.load();
}

const HidTransportOptions& HidTransport::options() const {
    return _options;
}

HidTransportStats HidTransport::stats() const {
    HidTransportStats current;
    current.readReports = _readReports.load();
    current.readBytes = _readBytes.load();
    current.acceptedReports = _acceptedReports.load();
    current.droppedReportId = _droppedReportId.load();
    current.readErrors = _readErrors.load();
    current.writeReports = _writeReports.load();
    current.writeBytes = _writeBytes.load();
    current.writeErrors = _writeErrors.load();
    {
        std::lock_guard<std::mutex> lock(_rxMutex);
        current.queuedReports = _rxQueue.size();
    }
    return current;
}

std::unique_ptr<IHidBackend> HidTransport::makeDefaultBackend() const {
    return std::make_unique<HidApiBackend>();
}

std::size_t HidTransport::inputReadBufferSize() const {
    const auto size = std::max(_options.inputReportSize, _options.readBufferSize);
#if defined(_WIN32) || defined(__APPLE__)
    if (_options.reportId == 0 && size < std::numeric_limits<std::size_t>::max()) {
        return size + 1;
    }
#endif
    return size;
}

std::size_t HidTransport::outputPayloadSize() const {
    return _options.outputReportSize > 0 ? _options.outputReportSize - 1 : 0;
}

void HidTransport::startReadThread() {
    if (!_options.useReadThread || _readThread.joinable()) {
        return;
    }
    _readStop.store(false);
    _readThread = std::thread([this]() { readLoop(); });
}

void HidTransport::stopReadThread() {
    _readStop.store(true);
    if (_readThread.joinable()) {
        _readThread.join();
    }
}

void HidTransport::readLoop() {
    if (_options.inputReportSize <= 1) {
        return;
    }

    Bytes report(inputReadBufferSize(), 0);
    while (!_readStop.load() && _open.load() && _backend != nullptr) {
        std::fill(report.begin(), report.end(), 0);
        const auto read =
            _backend->readReport(report.data(), report.size(), _options.readThreadTimeoutMs);
        if (!read.has_value()) {
            ++_readErrors;
            traceReport(HidReportTraceKind::ReadError,
                        nullptr,
                        0,
                        _options.readThreadTimeoutMs,
                        _backend->lastError());
        } else if (*read == 0) {
            traceReport(HidReportTraceKind::ReadTimeout, nullptr, 0, _options.readThreadTimeoutMs);
        } else if (*read > 0) {
            const auto readSize = std::min(*read, report.size());
            traceReport(
                HidReportTraceKind::ReadReport, report.data(), readSize, _options.readThreadTimeoutMs);
            handleReadReport(report.data(), readSize, true);
        }
    }
}

void HidTransport::traceReport(HidReportTraceKind kind,
                               const Byte* data,
                               std::size_t size,
                               std::uint32_t timeoutMs,
                               std::string message) const {
    if (!_options.reportTrace) {
        return;
    }

    HidReportTrace trace;
    trace.kind = kind;
    trace.data = data;
    trace.size = size;
    trace.reportId = data != nullptr && size > 0 ? data[0] : 0;
    trace.expectedReportId = _options.reportId;
    trace.timeoutMs = timeoutMs;
    trace.message = std::move(message);
    try {
        _options.reportTrace(trace);
    } catch (...) {
    }
}

void HidTransport::handleReadReport(const Byte* data, std::size_t size, bool queueForPoll) {
    if (data == nullptr || size == 0) {
        return;
    }

    ++_readReports;
    _readBytes.fetch_add(size);
    if (size <= 1 || data[0] != _options.reportId) {
        ++_droppedReportId;
        traceReport(HidReportTraceKind::DroppedReportId, data, size, 0);
        return;
    }

    ++_acceptedReports;
    traceReport(HidReportTraceKind::AcceptedReport, data, size, 0);
    if (queueForPoll) {
        std::lock_guard<std::mutex> lock(_rxMutex);
        _rxQueue.emplace(data + 1, data + size);
        return;
    }

    if (_sink != nullptr) {
        _sink->onBytes(data + 1, size - 1);
    }
}

void HidTransport::drainQueuedReports() {
    if (_sink == nullptr) {
        return;
    }

    for (std::size_t index = 0; index < _options.maxReportsPerPoll; ++index) {
        Bytes payload;
        {
            std::lock_guard<std::mutex> lock(_rxMutex);
            if (_rxQueue.empty()) {
                return;
            }
            payload = std::move(_rxQueue.front());
            _rxQueue.pop();
        }
        if (!payload.empty()) {
            _sink->onBytes(payload.data(), payload.size());
        }
    }
}

}  // namespace axtp
