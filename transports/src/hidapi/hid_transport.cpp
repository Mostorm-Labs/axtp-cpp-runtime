#include "hidapi/hid_transport.hpp"

#include <algorithm>
#include <cwchar>
#include <hidapi.h>
#include <utility>
#include <vector>

namespace axtp {
namespace {

class HidApiBackend : public IHidBackend {
public:
    ~HidApiBackend() override {
        close();
    }

    bool open(const HidTransportOptions& options) override {
        close();
        if (hid_init() != 0) {
            return false;
        }

        std::wstring serial;
        const wchar_t* serialPtr = nullptr;
        if (!options.serialNumber.empty()) {
            serial.assign(options.serialNumber.begin(), options.serialNumber.end());
            serialPtr = serial.c_str();
        }

        _handle = hid_open(options.vendorId, options.productId, serialPtr);
        if (_handle == nullptr) {
            hid_exit();
            return false;
        }
        hid_set_nonblocking(_handle, 1);
        _initialized = true;
        return true;
    }

    void close() override {
        if (_handle != nullptr) {
            hid_close(_handle);
            _handle = nullptr;
        }
        if (_initialized) {
            hid_exit();
            _initialized = false;
        }
    }

    bool writeReport(const Byte* data, std::size_t size) override {
        if (_handle == nullptr || data == nullptr || size == 0) {
            return false;
        }
        const auto written = hid_write(_handle, data, size);
        return written == static_cast<int>(size);
    }

    std::optional<std::size_t> readReport(Byte* data, std::size_t size) override {
        if (_handle == nullptr || data == nullptr || size == 0) {
            return std::nullopt;
        }
        const auto read = hid_read(_handle, data, size);
        if (read < 0) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(read);
    }

private:
    hid_device* _handle = nullptr;
    bool _initialized = false;
};

}  // namespace

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
    _open = _backend != nullptr && _backend->open(_options);
}

void HidTransport::close() {
    if (_backend != nullptr) {
        _backend->close();
    }
    _open = false;
}

void HidTransport::poll() {
    if (!_open || _backend == nullptr || _sink == nullptr || _options.inputReportSize <= 1) {
        return;
    }

    Bytes report(_options.inputReportSize, 0);
    for (std::size_t index = 0; index < _options.maxReportsPerPoll; ++index) {
        std::fill(report.begin(), report.end(), 0);
        const auto read = _backend->readReport(report.data(), report.size());
        if (!read.has_value() || *read == 0) {
            return;
        }
        if (report[0] != _options.reportId) {
            continue;
        }
        _sink->onBytes(report.data() + 1, report.size() - 1);
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

    Bytes report(_options.outputReportSize, 0);
    for (std::size_t offset = 0; offset < size;) {
        std::fill(report.begin(), report.end(), 0);
        report[0] = _options.reportId;
        const auto chunkSize = std::min(capacity, size - offset);
        std::copy(data + offset, data + offset + chunkSize, report.begin() + 1);
        if (!_backend->writeReport(report.data(), report.size())) {
            return;
        }
        offset += chunkSize;
    }
}

TransportProfile HidTransport::profile() const {
    TransportProfile profile;
    profile.kind = TransportKind::Hid;
    profile.wireMode = AxtpWireMode::FramedBinary;
    profile.defaultRpcEncoding = RpcEncoding::Tlv;
    profile.messageOriented = true;
    profile.supportsTextMessage = false;
    profile.supportsBinaryMessage = true;
    profile.preferredFrameSize = outputPayloadSize();
    return profile;
}

bool HidTransport::isOpen() const {
    return _open;
}

const HidTransportOptions& HidTransport::options() const {
    return _options;
}

std::unique_ptr<IHidBackend> HidTransport::makeDefaultBackend() const {
    return std::make_unique<HidApiBackend>();
}

std::size_t HidTransport::outputPayloadSize() const {
    return _options.outputReportSize > 0 ? _options.outputReportSize - 1 : 0;
}

}  // namespace axtp
