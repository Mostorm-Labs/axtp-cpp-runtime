#include "toolkit/axtp_toolkit.hpp"

#include <atomic>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

#if defined(_WIN32)
#    include <process.h>
#else
#    include <unistd.h>
#endif

#include "protocol/generated/registry_lookup.h"
#include "protocol/wire/websocket_json_rpc/outbound/json_rpc_encoder.hpp"
#include "runtime/testing/mock_transport.hpp"
#include "transports/tcp/boost/tcp_transport.hpp"
#include "transports/websocket/ix/websocket_transport.hpp"

namespace axtp::toolkit {
namespace {

int hexNibble(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

std::string formatTime(const char* pattern) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    char text[64] = {};
    std::strftime(text, sizeof(text), pattern, &tm);
    return text;
}

std::uint32_t processId() {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(_getpid());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

std::uint32_t generateRandomSeed() {
    std::random_device random;
    return (static_cast<std::uint32_t>(random()) << 16U) ^ static_cast<std::uint32_t>(random());
}

void logLine(const std::function<void(std::string_view)>& log, const std::string& line) {
    if (log) {
        log(line);
    }
}

}  // namespace

std::optional<std::uint32_t> parseUint32(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::size_t offset = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        offset = 2;
        base = 16;
    }
    if (offset >= text.size()) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(std::string(text.substr(offset)), &consumed, base);
        if (consumed == text.size() - offset &&
            value <= std::numeric_limits<std::uint32_t>::max()) {
            return static_cast<std::uint32_t>(value);
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::optional<Bytes> parseHex(std::string text) {
    std::string compact;
    compact.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') {
            continue;
        }
        if (ch == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
            ++i;
            continue;
        }
        compact.push_back(ch);
    }
    if (compact.size() % 2 != 0) {
        return std::nullopt;
    }
    Bytes bytes;
    bytes.reserve(compact.size() / 2);
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        const auto hi = hexNibble(compact[i]);
        const auto lo = hexNibble(compact[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<Byte>((hi << 4) | lo));
    }
    return bytes;
}

std::string toHex(const Bytes& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kDigits[(byte >> 4) & 0x0F]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::string toHexByte(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << (value & 0xFFU);
    return out.str();
}

std::string toHexId(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
        << value;
    return out.str();
}

std::string toHexU32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
        << value;
    return out.str();
}

std::string jsonEscape(std::string_view text) {
    return nlohmann::json(std::string(text)).dump();
}

const char* errorName(ErrorCode code) {
    const auto* descriptor = RegistryLookup::errorByCode(code);
    return descriptor != nullptr ? descriptor->name : "UNKNOWN_ERROR";
}

OutputFormat parseOutputFormat(std::string_view value) {
    if (value == "json") {
        return OutputFormat::Json;
    }
    if (value == "hex") {
        return OutputFormat::Hex;
    }
    if (value == "file") {
        return OutputFormat::File;
    }
    return OutputFormat::Pretty;
}

Logger::Logger(std::filesystem::path executablePath,
               std::string stem,
               bool enabled,
               bool includeBody) {
    open(std::move(executablePath), std::move(stem), enabled, includeBody);
}

void Logger::open(std::filesystem::path executablePath,
                  std::string stem,
                  bool enabled,
                  bool includeBody) {
    includeBody_ = includeBody;
    path_.clear();
    fileStream_.reset();
    if (!enabled) {
        return;
    }
    std::error_code ec;
    if (executablePath.empty()) {
        executablePath = std::filesystem::current_path(ec) / stem;
    }
    if (executablePath.is_relative()) {
        executablePath = std::filesystem::absolute(executablePath, ec);
    }
    auto dir = executablePath.parent_path() / (stem + "-logs");
    std::filesystem::create_directories(dir, ec);
    std::ostringstream name;
    name << stem << "-" << formatTime("%Y%m%d-%H%M%S") << "-" << processId() << ".log";
    path_ = dir / name.str();
    auto stream = std::make_unique<std::ofstream>(path_, std::ios::out | std::ios::app);
    if (*stream) {
        fileStream_ = std::move(stream);
        write("log opened");
    } else {
        path_.clear();
    }
}

void Logger::write(std::string_view line) {
    if (!fileStream_) {
        return;
    }
    *fileStream_ << formatTime("%Y-%m-%d %H:%M:%S") << " " << line << "\n";
    fileStream_->flush();
}

bool Logger::includeBody() const {
    return includeBody_;
}

const std::filesystem::path& Logger::path() const {
    return path_;
}

std::string Logger::pathString() const {
    return path_.string();
}

bool hasHidTarget(const HidOpenOptions& options) {
    return !options.devicePath.empty() ||
           (options.vendorId.has_value() && options.productId.has_value());
}

HidTransportOptions makeHidTransportOptions(const HidOpenOptions& options) {
    HidTransportOptions hid;
    hid.vendorId = static_cast<std::uint16_t>(options.vendorId.value_or(0));
    hid.productId = static_cast<std::uint16_t>(options.productId.value_or(0));
    hid.usagePage = static_cast<std::uint16_t>(options.usagePage.value_or(0));
    hid.usage = static_cast<std::uint16_t>(options.usage.value_or(0));
    hid.devicePath = options.devicePath;
    hid.serialNumber = options.serialNumber;
    hid.reportId = static_cast<std::uint8_t>(options.reportId);
    hid.inputReportSize = static_cast<std::size_t>(options.inputReportSize);
    hid.readBufferSize = static_cast<std::size_t>(options.readBufferSize);
    hid.outputReportSize = static_cast<std::size_t>(options.outputReportSize);
    hid.maxReportsPerPoll = static_cast<std::size_t>(options.maxReportsPerPoll);
    hid.useReadThread = options.useReadThread;
    hid.readThreadTimeoutMs = options.readThreadTimeoutMs;
    hid.reportTrace = options.reportTrace;
    return hid;
}

bool matchesHidDeviceFilters(const HidDeviceInfo& device, const HidOpenOptions& options) {
    if (options.usagePage.value_or(0) != 0 && device.usagePage != *options.usagePage) {
        return false;
    }
    if (options.usage.value_or(0) != 0 && device.usage != *options.usage) {
        return false;
    }
    return true;
}

std::vector<HidDeviceInfo> listHidDevices(const HidOpenOptions& options) {
    const auto allDevices =
        enumerateHidDevices(static_cast<std::uint16_t>(options.vendorId.value_or(0)),
                            static_cast<std::uint16_t>(options.productId.value_or(0)));
    std::vector<HidDeviceInfo> devices;
    for (const auto& device : allDevices) {
        if (matchesHidDeviceFilters(device, options)) {
            devices.push_back(device);
        }
    }
    return devices;
}

void printHidDevices(const HidOpenOptions& options, OutputFormat format, std::ostream& out) {
    const auto devices = listHidDevices(options);
    if (format == OutputFormat::Json) {
        auto json = nlohmann::json::array();
        for (const auto& device : devices) {
            auto item = nlohmann::json::object();
            item["path"] = device.path;
            item["vendorId"] = device.vendorId;
            item["productId"] = device.productId;
            item["releaseNumber"] = device.releaseNumber;
            item["serialNumber"] = device.serialNumber;
            item["manufacturer"] = device.manufacturer;
            item["product"] = device.product;
            item["usagePage"] = device.usagePage;
            item["usage"] = device.usage;
            item["interfaceNumber"] = device.interfaceNumber;
            item["busType"] = device.busType;
            json.push_back(std::move(item));
        }
        out << json.dump() << "\n";
        return;
    }

    for (const auto& device : devices) {
        out << "path=" << device.path
            << " vid=" << toHexId(device.vendorId)
            << " pid=" << toHexId(device.productId)
            << " serial=" << (device.serialNumber.empty() ? "<none>" : device.serialNumber)
            << " manufacturer=" << (device.manufacturer.empty() ? "<none>" : device.manufacturer)
            << " product=" << (device.product.empty() ? "<none>" : device.product)
            << " usagePage=" << toHexId(device.usagePage)
            << " usage=" << toHexId(device.usage)
            << " interface=" << device.interfaceNumber
            << " bus=" << (device.busType.empty() ? "<none>" : device.busType)
            << "\n";
    }
}

TransportBundle makeTransport(const TransportOpenOptions& options) {
    TransportBundle bundle;
    if (options.kind == "mock") {
        bundle.transport = std::make_unique<MockTransport>();
        return bundle;
    }
    if (options.kind == "tcp") {
        bundle.transport = std::make_unique<TcpTransport>(
            static_cast<std::uint16_t>(options.port.value_or(0)), options.host.c_str());
        return bundle;
    }
    if (options.kind == "websocket" || options.kind == "ws") {
        bundle.transport = std::make_unique<WebSocketTransport>(
            static_cast<std::uint16_t>(options.port.value_or(0)), options.host.c_str());
        return bundle;
    }
    if (options.kind == "hid" || options.kind == "hidapi") {
        auto transport = std::make_unique<HidTransport>(makeHidTransportOptions(options.hid));
        bundle.hidTransport = transport.get();
        bundle.transport = std::move(transport);
        return bundle;
    }
    return bundle;
}

EndpointAppReadyResult ensureEndpointAppReady(AxtpEndpoint<BasicBroker<>>& endpoint,
                                              ITransport& transport,
                                              EndpointAppReadyOptions options) {
    EndpointAppReadyResult result;
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    const auto profile = transport.profile();
    endpoint.core().configure(profile);

    if (profile.wireMode == AxtpWireMode::FramedBinary) {
        result.stage = "control-open";
        const std::uint16_t controlId = 1;
        logLine(options.log, "APP_READY control: send CONTROL OPEN");
        endpoint.sendControlOpen(controlId);
        logLine(options.log, "APP_READY control: wait CONTROL ACCEPT");
        while (std::chrono::steady_clock::now() < deadline) {
            endpoint.poll(32);
            if (auto accept = endpoint.tryTakeControlNotice(ControlOpcode::Accept)) {
                std::ostringstream out;
                out << "APP_READY control: ACCEPT controlId=" << accept->controlId
                    << " status=" << errorName(accept->statusCode);
                logLine(options.log, out.str());
                if (accept->controlId == controlId &&
                    accept->statusCode == ErrorCode::Success &&
                    endpoint.core().controlSessionOpen()) {
                    break;
                }
                result.status = accept->statusCode == ErrorCode::Success
                                    ? ErrorCode::ControlOpenRejected
                                    : accept->statusCode;
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!endpoint.core().controlSessionOpen()) {
            result.status = ErrorCode::RpcResponseTimeout;
            return result;
        }
    }

    result.stage = "hello";
    logLine(options.log, "APP_READY rpc: wait Hello");
    bool gotHello = false;
    while (std::chrono::steady_clock::now() < deadline) {
        endpoint.poll(32);
        if (auto hello = endpoint.tryTakeSessionRpc(RpcOp::Hello)) {
            gotHello = true;
            if (options.includeBody) {
                logLine(options.log,
                        "APP_READY rpc: Hello body=" +
                            std::string(hello->body.begin(), hello->body.end()));
            } else {
                logLine(options.log, "APP_READY rpc: Hello received");
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!gotHello) {
        result.status = ErrorCode::RpcResponseTimeout;
        return result;
    }

    result.hasRandomSeed = true;
    result.randomSeed = options.randomSeed.value_or(generateRandomSeed());
    result.stage = "identify";
    logLine(options.log,
            "APP_READY rpc: send Identify randomSeed=" + toHexU32(result.randomSeed));
    endpoint.sendRpcSession(JsonRpcEncoder::makeIdentify(result.randomSeed, ""));

    result.stage = "identified";
    logLine(options.log, "APP_READY rpc: wait Identified");
    while (std::chrono::steady_clock::now() < deadline) {
        endpoint.poll(32);
        if (auto identified = endpoint.tryTakeSessionRpc(RpcOp::Identified)) {
            result.sid = identified->meta.jsonSid;
            if (options.includeBody) {
                logLine(options.log,
                        "APP_READY rpc: Identified sid=" + result.sid + " body=" +
                            std::string(identified->body.begin(), identified->body.end()));
            } else {
                logLine(options.log, "APP_READY rpc: Identified sid=" + result.sid);
            }
            if (result.sid.empty()) {
                result.status = ErrorCode::RpcPayloadInvalid;
                return result;
            }
            result.stage = "app-ready";
            result.ok = true;
            result.status = ErrorCode::Success;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    result.status = ErrorCode::RpcResponseTimeout;
    return result;
}

}  // namespace axtp::toolkit
