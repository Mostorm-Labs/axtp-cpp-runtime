#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "core/protocol/model/bytes.hpp"
#include "core/protocol/model/error.hpp"
#include "core/runtime/broker/basic_broker.hpp"
#include "core/runtime/endpoint/axtp_endpoint.hpp"
#include "core/runtime/transport/transport.hpp"
#include "sdk/app_ready_options.hpp"
#include "transports/hidapi/hid_transport.hpp"

// Internal support library for repository tools. This is not a stable SDK API.
namespace axtp::toolkit {

enum class OutputFormat {
    Pretty,
    Json,
    Hex,
    File,
};

std::optional<std::uint32_t> parseUint32(std::string_view text);
std::optional<Bytes> parseHex(std::string text);
std::string toHex(const Bytes& bytes);
std::string toHexByte(std::uint32_t value);
std::string toHexId(std::uint32_t value);
std::string toHexU32(std::uint32_t value);
std::string jsonEscape(std::string_view text);
const char* errorName(ErrorCode code);
OutputFormat parseOutputFormat(std::string_view value);

class Logger {
public:
    Logger() = default;
    Logger(std::filesystem::path executablePath,
           std::string stem,
           bool enabled,
           bool includeBody);

    void open(std::filesystem::path executablePath,
              std::string stem,
              bool enabled,
              bool includeBody);
    void write(std::string_view line);
    bool includeBody() const;
    const std::filesystem::path& path() const;
    std::string pathString() const;

private:
    bool includeBody_ = false;
    std::filesystem::path path_;
    std::unique_ptr<std::ofstream> fileStream_;
};

struct HidOpenOptions {
    std::optional<std::uint32_t> vendorId;
    std::optional<std::uint32_t> productId;
    std::optional<std::uint32_t> usagePage;
    std::optional<std::uint32_t> usage;
    std::string devicePath;
    std::string serialNumber;
    std::uint32_t reportId = 0x05;
    std::uint32_t inputReportSize = 255;
    std::uint32_t readBufferSize = 4096;
    std::uint32_t outputReportSize = 255;
    std::uint32_t maxReportsPerPoll = 16;
    bool useReadThread = true;
    std::uint32_t readThreadTimeoutMs = 1000;
    std::function<void(const HidReportTrace&)> reportTrace;
};

bool hasHidTarget(const HidOpenOptions& options);
HidTransportOptions makeHidTransportOptions(const HidOpenOptions& options);
std::vector<HidDeviceInfo> listHidDevices(const HidOpenOptions& options);
void printHidDevices(const HidOpenOptions& options, OutputFormat format, std::ostream& out);

struct TransportOpenOptions {
    std::string kind = "mock";
    std::string host = "127.0.0.1";
    std::optional<std::uint32_t> port;
    HidOpenOptions hid;
};

struct TransportBundle {
    std::unique_ptr<ITransport> transport;
    HidTransport* hidTransport = nullptr;
};

TransportBundle makeTransport(const TransportOpenOptions& options);

struct EndpointAppReadyOptions {
    std::chrono::milliseconds timeout{5000};
    std::optional<std::uint32_t> randomSeed;
    bool includeBody = false;
    std::function<void(std::string_view)> log;
};

struct EndpointAppReadyResult {
    bool ok = false;
    ErrorCode status = ErrorCode::Success;
    std::string stage;
    std::string sid;
    bool hasRandomSeed = false;
    std::uint32_t randomSeed = 0;
};

EndpointAppReadyResult ensureEndpointAppReady(AxtpEndpoint<BasicBroker<>>& endpoint,
                                              ITransport& transport,
                                              EndpointAppReadyOptions options);

}  // namespace axtp::toolkit
