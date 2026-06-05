#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "transport/transport_profile.hpp"

namespace axtp::sdk {

struct TcpEndpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
};

struct WebSocketEndpoint {
    std::string url;
    AxtpWireMode wireMode = AxtpWireMode::WebSocketJsonRpc;
};

struct HidEndpoint {
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::string serialNumber;
    std::uint8_t reportId = 0;
    std::size_t inputReportSize = 64;
    std::size_t outputReportSize = 64;
    std::size_t maxReportsPerPoll = 16;
};

struct BleEndpoint {
    std::string deviceId;
    std::string serviceUuid;
    std::string characteristicUuid;
};

struct UartEndpoint {
    std::string path;
    std::uint32_t baudRate = 115200;
};

}  // namespace axtp::sdk
