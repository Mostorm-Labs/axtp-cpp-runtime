#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    include <process.h>
#    include <stdlib.h>
#else
#    include <unistd.h>
#endif

#include "axtp_client.hpp"
#include "generated/axtp_method_registry_generated.h"
#include "generated/registry_lookup.h"
#include "hidapi/hid_transport.hpp"

namespace {

enum class OutputFormat {
    Text,
    Json,
    Hex,
};

struct CliOptions {
    std::string transport = "hid";
    std::string output = "text";
    std::string hidPath;
    std::string serialNumber;
    std::string sid = "00000000";
    std::optional<std::string> commandMethod;
    std::optional<std::string> json;
    std::optional<std::string> jsonFile;
    std::optional<std::string> rawHex;
    std::optional<std::string> rawFile;
    std::optional<std::string> tlvHex;
    std::optional<std::string> tlvFile;
    std::optional<std::uint32_t> methodId;
    std::optional<std::uint32_t> vid;
    std::optional<std::uint32_t> pid;
    std::optional<std::uint32_t> randomSeed;
    std::uint32_t timeoutMs = 5000;
    std::uint32_t reportId = 0x05;
    std::uint32_t inputReportSize = 255;
    std::uint32_t readBufferSize = 4096;
    std::uint32_t outputReportSize = 255;
    std::uint32_t maxReportsPerPoll = 16;
    bool logEnabled = true;
    bool logBody = false;
    bool noAppReady = false;
    bool help = false;
    std::vector<std::string> positional;
};

void printUsage() {
    std::cout
        << "Usage: axtpctl [options] call <method>\n"
        << "       axtpctl -c <method> [--json JSON|--json-file FILE]\n"
        << "\n"
        << "HID options:\n"
        << "  -t, --transport hid          HID transport only in this build\n"
        << "      --vid <hex|dec>          HID vendor id, for example 0x1234\n"
        << "      --pid <hex|dec>          HID product id, for example 0x5678\n"
        << "      --path, --hid-path <path> Open HIDAPI path from list-hid with hid_open_path\n"
        << "      --serial <value>         HID serial value for VID/PID open\n"
        << "      --report-id <id>         HID report id, default 0x05\n"
        << "      --input-report-size <n>  HIDAPI input buffer bytes incl report id, default 255\n"
        << "      --read-buffer-size <n>   HIDAPI read buffer bytes, default 4096\n"
        << "      --output-report-size <n> HIDAPI output buffer bytes incl report id, default 255\n"
        << "      --max-reports-per-poll <n> HID read limit per poll, default 16\n"
        << "                                HID RX pump thread uses 1000ms blocking reads\n"
        << "\n"
        << "Call options:\n"
        << "  -c, --command <method>       Call an AXTP method by name\n"
        << "      --method-id <hex|dec>    Call an AXTP method by numeric id\n"
        << "      --sid <value>            JSON envelope sid, default 00000000\n"
        << "      --no-app-ready           Skip CONTROL/Hello/Identify and use --sid directly\n"
        << "      --random-seed <hex|dec>  Override Identify randomSeed for app-ready\n"
        << "  -j, --json <json>            JSON request body, default {}\n"
        << "  -f, --json-file <path>       Read JSON request body from file\n"
        << "      --raw-hex <hex>          Send raw binary body as JSON-binary RPC\n"
        << "      --raw-file <path>        Read raw binary body from file\n"
        << "      --tlv-hex <hex>          Send TLV body as JSON-binary RPC\n"
        << "      --tlv-file <path>        Read TLV body from file\n"
        << "      --timeout <ms>           RPC timeout or read-hid duration; 0 means forever for read-hid\n"
        << "  -o, --output <text|json|hex> Output format, default text\n"
        << "  -h, --help                   Show this help\n"
        << "\n"
        << "Logging options:\n"
        << "      --log                    Write a local log under <exe-dir>\\axtpctl-logs (default)\n"
        << "      --no-log                 Disable local file logging\n"
        << "      --log-body               Also log request/response body; may contain secrets\n"
        << "\n"
        << "Commands:\n"
        << "  call <method>\n"
        << "  handshake                 Open HID and complete CONTROL + RPC app-ready\n"
        << "  read-hid                  Open HID and only read inbound reports\n"
        << "  list-hid [--vid VID --pid PID]\n"
        << "  list-methods\n"
        << "  capability methods\n"
        << "\n"
        << "Examples:\n"
        << "  axtpctl list-hid --vid 0x1234 --pid 0x5678\n"
        << "  axtpctl read-hid --path \"<path from list-hid>\" --timeout 10000 --log-body\n"
        << "  axtpctl --path \"<path from list-hid>\" -c network.getInterfaces -o json\n"
        << "  axtpctl --path \"<path from list-hid>\" handshake --random-seed 0x12345678\n"
        << "  axtpctl --path \"<path from list-hid>\" -c network.getInterfaces --no-app-ready --sid 00000000\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 -c audio.getAlgorithmConfig\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 -c audio.setAlgorithmConfig --json \"{}\"\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 call --method-id 0x0901 -o json\n"
        << "\n"
        << "Network examples:\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 -c network.getInterfaces --json "
           "'{\"includeDisabled\":true}' -o json\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 -c network.getInterfaceInfo --json "
           "'{\"interfaceId\":\"wifi-sta0\"}' -o json\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 -c network.setIpConfig --json "
           "'{\"interfaceId\":\"eth0\",\"family\":\"ipv4\",\"config\":{\"mode\":\"dhcp\"},"
           "\"applyPolicy\":\"immediate\"}'\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 -c network.setWifiConfig --json "
           "'{\"interfaceId\":\"wifi-sta0\",\"profile\":{\"ssid\":\"Lab-AP\","
           "\"securityType\":\"wpa2_psk\",\"credential\":{\"type\":\"passphrase\","
           "\"secretRef\":\"wifi-passphrase\"},\"persist\":true},\"replaceExisting\":true,"
           "\"makeDefault\":true,\"connectAfterSave\":true}'\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 -c network.connectWifi --json "
           "'{\"interfaceId\":\"wifi-sta0\",\"profileId\":\"Lab-AP\",\"timeoutMs\":15000}'\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 -c network.setApConfig --json "
           "'{\"interfaceId\":\"ap0\",\"config\":{\"enabled\":true,\"ssid\":\"AXTP-AP\","
           "\"securityType\":\"wpa2_psk\",\"credential\":{\"type\":\"passphrase\","
           "\"secretRef\":\"ap-passphrase\"},\"band\":\"5g\",\"channel\":149,"
           "\"maxClients\":8}}'\n"
        << "  axtpctl --vid 0x1234 --pid 0x5678 -c network.startAp --json "
           "'{\"interfaceId\":\"ap0\"}'\n";
}

bool isOption(std::string_view text) {
    return !text.empty() && text.front() == '-';
}

std::optional<std::uint32_t> parseUint32(const std::string& text) {
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
        const auto value = std::stoull(text.substr(offset), &consumed, base);
        if (consumed != text.size() - offset || value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

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

std::optional<axtp::Bytes> parseHex(std::string text) {
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

    axtp::Bytes bytes;
    bytes.reserve(compact.size() / 2);
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        const auto hi = hexNibble(compact[i]);
        const auto lo = hexNibble(compact[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<axtp::Byte>((hi << 4) | lo));
    }
    return bytes;
}

std::string toHex(const axtp::Bytes& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        output.push_back(kDigits[(byte >> 4) & 0x0F]);
        output.push_back(kDigits[byte & 0x0F]);
    }
    return output;
}

std::string toHexId(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << value;
    return out.str();
}

std::string toHexByte(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << (value & 0xFF);
    return out.str();
}

std::string toHexU32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string jsonEscape(std::string_view text) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(text.size() + 8);
    for (const auto raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20 || ch >= 0x7F) {
                output += "\\u00";
                output.push_back(kDigits[(ch >> 4) & 0x0F]);
                output.push_back(kDigits[ch & 0x0F]);
            } else {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return output;
}

std::optional<std::string> readTextFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::optional<axtp::Bytes> readBinaryFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return axtp::Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

OutputFormat parseOutputFormat(const std::string& value) {
    if (value == "json") {
        return OutputFormat::Json;
    }
    if (value == "hex") {
        return OutputFormat::Hex;
    }
    return OutputFormat::Text;
}

const char* encodingName(axtp::RpcEncoding encoding) {
    if (encoding == axtp::RpcEncoding::Json) {
        return "json";
    }
    if (axtp::isJsonBinaryRpcEncoding(encoding)) {
        return "json-binary";
    }
    if (encoding == axtp::RpcEncoding::Cbor) {
        return "cbor";
    }
    if (encoding == axtp::RpcEncoding::Msgpack) {
        return "msgpack";
    }
    return "unknown";
}

const char* errorName(axtp::ErrorCode code) {
    const auto* descriptor = axtp::RegistryLookup::errorByCode(code);
    return descriptor != nullptr ? descriptor->name : "ERROR";
}

bool isMostlyText(const axtp::Bytes& bytes) {
    for (const auto byte : bytes) {
        if (byte == '\n' || byte == '\r' || byte == '\t') {
            continue;
        }
        if (byte < 0x20) {
            return false;
        }
    }
    return true;
}

std::tm localTime(std::time_t value) {
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &value);
#else
    localtime_r(&value, &out);
#endif
    return out;
}

std::string formatTime(const char* format) {
    const auto now = std::time(nullptr);
    const auto tm = localTime(now);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return buffer;
}

std::uint32_t processId() {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(_getpid());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

std::filesystem::path executablePath(const char* argv0) {
#if defined(_WIN32)
    char* programPath = nullptr;
    if (_get_pgmptr(&programPath) == 0 && programPath != nullptr && programPath[0] != '\0') {
        return std::filesystem::path(programPath);
    }
#else
    std::vector<char> buffer(4096, 0);
    const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size > 0) {
        return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(size)));
    }
#endif
    std::error_code ec;
    auto path = std::filesystem::absolute(argv0 != nullptr ? argv0 : "axtpctl", ec);
    if (!ec) {
        return path;
    }
    return std::filesystem::current_path() / "axtpctl";
}

bool isLoggedBodyOption(const std::string& arg) {
    return arg == "--json" || arg == "-j" || arg == "--raw-hex" || arg == "--raw-file" ||
           arg == "--tlv-hex" || arg == "--tlv-file";
}

std::string sanitizedArgv(int argc, char** argv, bool includeBody) {
    std::ostringstream out;
    bool redactNext = false;
    for (int i = 0; i < argc; ++i) {
        if (i != 0) {
            out << ' ';
        }
        const std::string arg = argv[i] != nullptr ? argv[i] : "";
        if (redactNext && !includeBody) {
            out << "<redacted>";
            redactNext = false;
            continue;
        }
        out << arg;
        redactNext = isLoggedBodyOption(arg);
    }
    return out.str();
}

std::string bytesForLog(const axtp::Bytes& bytes, axtp::RpcEncoding encoding, bool includeBody) {
    std::ostringstream out;
    out << bytes.size() << " bytes";
    if (!includeBody) {
        return out.str();
    }
    if (bytes.empty()) {
        return out.str();
    }
    if (encoding == axtp::RpcEncoding::Json || isMostlyText(bytes)) {
        out << " text=" << std::string(bytes.begin(), bytes.end());
        return out.str();
    }
    out << " hex=" << toHex(bytes);
    return out.str();
}

std::uint16_t readBe16(const axtp::Byte* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t readBe32(const axtp::Byte* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

const char* payloadTypeName(std::uint8_t value) {
    if (value == static_cast<std::uint8_t>(axtp::PayloadType::Control)) {
        return "control";
    }
    if (value == static_cast<std::uint8_t>(axtp::PayloadType::Rpc)) {
        return "rpc";
    }
    if (value == static_cast<std::uint8_t>(axtp::PayloadType::Stream)) {
        return "stream";
    }
    return "unknown";
}

const char* rpcOpName(std::uint8_t value) {
    switch (static_cast<axtp::RpcOp>(value)) {
    case axtp::RpcOp::Hello:
        return "hello";
    case axtp::RpcOp::Identify:
        return "identify";
    case axtp::RpcOp::Identified:
        return "identified";
    case axtp::RpcOp::Event:
        return "event";
    case axtp::RpcOp::Request:
        return "request";
    case axtp::RpcOp::RequestResponse:
        return "request-response";
    case axtp::RpcOp::RequestBatch:
        return "request-batch";
    case axtp::RpcOp::RequestBatchResponse:
        return "request-batch-response";
    default:
        return "unknown";
    }
}

const char* controlOpcodeName(std::uint8_t value) {
    switch (static_cast<axtp::ControlOpcode>(value)) {
    case axtp::ControlOpcode::Open:
        return "open";
    case axtp::ControlOpcode::Accept:
        return "accept";
    case axtp::ControlOpcode::Ready:
        return "ready";
    case axtp::ControlOpcode::Heartbeat:
        return "heartbeat";
    case axtp::ControlOpcode::HeartbeatAck:
        return "heartbeat-ack";
    case axtp::ControlOpcode::Ack:
        return "ack";
    case axtp::ControlOpcode::Nack:
        return "nack";
    case axtp::ControlOpcode::Resume:
        return "resume";
    case axtp::ControlOpcode::ResumeAck:
        return "resume-ack";
    case axtp::ControlOpcode::SessionReset:
        return "session-reset";
    case axtp::ControlOpcode::WindowUpdate:
        return "window-update";
    case axtp::ControlOpcode::Ping:
        return "ping";
    case axtp::ControlOpcode::Pong:
        return "pong";
    case axtp::ControlOpcode::Close:
        return "close";
    case axtp::ControlOpcode::CloseAck:
        return "close-ack";
    case axtp::ControlOpcode::Goaway:
        return "goaway";
    default:
        return "unknown";
    }
}

const char* bodyEncodingName(std::uint8_t value) {
    switch (static_cast<axtp::RpcBodyEncoding>(value)) {
    case axtp::RpcBodyEncoding::None:
        return "none";
    case axtp::RpcBodyEncoding::Tlv8:
        return "tlv8";
    case axtp::RpcBodyEncoding::Tlv16:
        return "tlv16";
    }
    return "unknown";
}

bool isJsonStart(axtp::Byte value) {
    return value == static_cast<axtp::Byte>('{') || value == static_cast<axtp::Byte>('[');
}

void appendControlTlvForLog(std::ostringstream& out,
                            const axtp::Bytes& bytes,
                            bool includeBody) {
    const auto tlv = axtp::ControlTlvCodec::decode(bytes);
    out << " control.tlv.valid=" << (tlv.valid ? "true" : "false");
    if (tlv.hasSessionId) {
        out << " control.tlv.sessionId=" << toHexU32(tlv.sessionId);
    }
    if (tlv.hasProtocolVersion) {
        out << " control.tlv.protocolVersion=" << static_cast<unsigned>(tlv.protocolVersion);
    }
    if (tlv.hasMaxFrameSize) {
        out << " control.tlv.maxFrameSize=" << tlv.maxFrameSize;
    }
    if (tlv.hasMtu) {
        out << " control.tlv.mtu=" << tlv.mtu;
    }
    if (tlv.hasSupportedPayloadTypes) {
        out << " control.tlv.supportedPayloadTypes=" << toHexByte(tlv.supportedPayloadTypes);
    }
    if (tlv.hasSupportedRpcEncodings) {
        out << " control.tlv.supportedRpcEncodings=" << toHexByte(tlv.supportedRpcEncodings);
    }
    if (tlv.hasSelectedRpcEncoding) {
        out << " control.tlv.selectedRpcEncoding=" << toHexByte(tlv.selectedRpcEncoding);
    }
    if (tlv.hasHeartbeatIntervalMs) {
        out << " control.tlv.heartbeatIntervalMs=" << tlv.heartbeatIntervalMs;
    }
    if (tlv.hasAckMode) {
        out << " control.tlv.ackMode=" << static_cast<unsigned>(tlv.ackMode);
    }
    if (includeBody && !bytes.empty()) {
        out << " control.tlv.hex=" << toHex(bytes);
    }
}

void appendFrameForLog(std::ostringstream& out,
                       const axtp::Byte* data,
                       std::size_t size,
                       bool includeBody) {
    out << " frame.size=" << size;
    if (data == nullptr || size < axtp::kStandardFrameHeaderSize) {
        out << " frameHeader=<incomplete>";
        return;
    }

    out << " headerHex=" << toHex(axtp::Bytes(data, data + axtp::kStandardFrameHeaderSize));
    if (data[0] != axtp::kAxtpStandardMagic0 || data[1] != axtp::kAxtpStandardMagic1) {
        out << " header.magic=<unexpected>";
        if (includeBody) {
            out << " frameHex=" << toHex(axtp::Bytes(data, data + size));
        }
        return;
    }

    const auto version = data[2];
    const auto payloadType = data[3];
    const auto payloadLength = readBe16(data + 4);
    const auto sourceId = data[6];
    const auto destinationId = data[7];
    const auto messageId = readBe16(data + 8);
    const auto frameIndex = data[10];
    const auto frameCount = data[11];
    const std::size_t totalSize = axtp::kStandardFrameHeaderSize +
                                  static_cast<std::size_t>(payloadLength) +
                                  axtp::kStandardFrameCrcSize;
    out << " header.magic=0x" << toHex(axtp::Bytes(data, data + 2))
        << " header.version=" << static_cast<unsigned>(version)
        << " header.payloadType=" << payloadTypeName(payloadType) << "("
        << static_cast<unsigned>(payloadType) << ")"
        << " header.payloadLength=" << payloadLength
        << " header.sourceId=" << static_cast<unsigned>(sourceId)
        << " header.destinationId=" << static_cast<unsigned>(destinationId)
        << " header.messageId=" << toHexId(messageId)
        << " header.frameIndex=" << static_cast<unsigned>(frameIndex)
        << " header.frameCount=" << static_cast<unsigned>(frameCount)
        << " frame.totalSize=" << totalSize;

    if (size >= totalSize) {
        out << " frame.crc16=" << toHexId(readBe16(data + totalSize - axtp::kStandardFrameCrcSize));
    } else {
        out << " frame.crc16=<incomplete>";
    }

    if (payloadType == static_cast<std::uint8_t>(axtp::PayloadType::Control)) {
        if (size < totalSize || payloadLength < axtp::kControlPayloadHeaderSize) {
            out << " control.header=<incomplete>";
            return;
        }
        const auto* control = data + axtp::kStandardFrameHeaderSize;
        const auto opcode = control[0];
        const auto controlId = readBe16(control + 1);
        const auto statusCode = readBe16(control + 3);
        out << " control.opcode=" << controlOpcodeName(opcode) << "("
            << static_cast<unsigned>(opcode) << ")"
            << " control.controlId=" << controlId
            << " control.statusCode=" << statusCode;
        const auto* tlvBody = control + axtp::kControlPayloadHeaderSize;
        const auto tlvSize =
            static_cast<std::size_t>(payloadLength - axtp::kControlPayloadHeaderSize);
        appendControlTlvForLog(out, axtp::Bytes(tlvBody, tlvBody + tlvSize), includeBody);
        return;
    }

    if (payloadType != static_cast<std::uint8_t>(axtp::PayloadType::Rpc) ||
        size < totalSize || payloadLength == 0) {
        return;
    }

    const auto* rpc = data + axtp::kStandardFrameHeaderSize;
    const auto encoding = static_cast<axtp::RpcEncoding>(rpc[0]);
    out << " rpc.encoding=" << encodingName(encoding);

    if (payloadLength >= 2 && encoding == axtp::RpcEncoding::Json && isJsonStart(rpc[1])) {
        const auto* json = rpc + 1;
        const auto jsonSize = static_cast<std::size_t>(payloadLength - 1);
        const axtp::Bytes jsonBytes(json, json + jsonSize);
        out << " rpc.wire=json-envelope jsonBytes=" << jsonSize
            << " json=" << std::string(jsonBytes.begin(), jsonBytes.end());
        return;
    }

    if (payloadLength < axtp::kBinaryRpcHeaderSize) {
        out << " rpc.header=<incomplete>";
        if (includeBody) {
            const axtp::Bytes payloadBytes(rpc, rpc + payloadLength);
            out << " payloadHex=" << toHex(payloadBytes);
        }
        return;
    }

    const auto op = rpc[1];
    const auto requestId = readBe32(rpc + 2);
    const auto methodId = readBe16(rpc + 6);
    const auto statusCode = readBe16(rpc + 8);
    const auto bodyEncoding = rpc[10];
    const auto* body = rpc + axtp::kBinaryRpcHeaderSize;
    const auto bodySize = static_cast<std::size_t>(payloadLength - axtp::kBinaryRpcHeaderSize);
    out << " rpc.wire=binary-header"
        << " rpc.op=" << rpcOpName(op) << "(" << static_cast<unsigned>(op) << ")"
        << " rpc.requestId=" << requestId
        << " rpc.methodId=" << toHexId(methodId)
        << " rpc.statusCode=" << statusCode
        << " rpc.bodyEncoding=" << bodyEncodingName(bodyEncoding)
        << " jsonBytes=" << bodySize;
    if (bodySize > 0) {
        const axtp::Bytes bodyBytes(body, body + bodySize);
        if (encoding == axtp::RpcEncoding::Json) {
            out << " json=" << std::string(bodyBytes.begin(), bodyBytes.end());
        } else if (includeBody && isMostlyText(bodyBytes)) {
            out << " text=" << std::string(bodyBytes.begin(), bodyBytes.end());
        } else if (includeBody) {
            out << " bodyHex=" << toHex(bodyBytes);
        }
    }
}

std::string txFrameForLog(const axtp::HidReportTrace& trace, bool includeBody) {
    std::ostringstream out;
    out << "hid tx frame";
    appendFrameForLog(out, trace.data, trace.size, includeBody);
    return out.str();
}

std::string rxFrameForLog(const axtp::HidReportTrace& trace, bool includeBody) {
    std::ostringstream out;
    out << "hid rx frame reportSize=" << trace.size
        << " reportId=" << toHexByte(trace.reportId)
        << " expectedReportId=" << toHexByte(trace.expectedReportId);
    if (!trace.message.empty()) {
        out << " message=" << trace.message;
    }
    if (trace.data == nullptr || trace.size <= 1) {
        out << " frameHeader=<missing>";
        return out.str();
    }
    appendFrameForLog(out, trace.data + 1, trace.size - 1, includeBody);
    if (includeBody) {
        out << " reportHex=" << toHex(axtp::Bytes(trace.data, trace.data + trace.size));
    }
    return out.str();
}

const char* hidTraceKindName(axtp::HidReportTraceKind kind) {
    switch (kind) {
    case axtp::HidReportTraceKind::ReadReport:
        return "report";
    case axtp::HidReportTraceKind::ReadTimeout:
        return "timeout";
    case axtp::HidReportTraceKind::ReadError:
        return "error";
    case axtp::HidReportTraceKind::WriteFrame:
        return "frame";
    case axtp::HidReportTraceKind::WriteReport:
        return "report";
    case axtp::HidReportTraceKind::WriteError:
        return "error";
    case axtp::HidReportTraceKind::AcceptedReport:
        return "accepted";
    case axtp::HidReportTraceKind::DroppedReportId:
        return "dropped-report-id";
    }
    return "unknown";
}

const char* hidTraceDirectionName(axtp::HidReportTraceKind kind) {
    switch (kind) {
    case axtp::HidReportTraceKind::WriteFrame:
    case axtp::HidReportTraceKind::WriteReport:
    case axtp::HidReportTraceKind::WriteError:
        return "tx";
    case axtp::HidReportTraceKind::ReadReport:
    case axtp::HidReportTraceKind::ReadTimeout:
    case axtp::HidReportTraceKind::ReadError:
    case axtp::HidReportTraceKind::AcceptedReport:
    case axtp::HidReportTraceKind::DroppedReportId:
        return "rx";
    }
    return "trace";
}

std::string hidTraceForLog(const axtp::HidReportTrace& trace, bool includeBody) {
    if (trace.kind == axtp::HidReportTraceKind::WriteFrame) {
        return txFrameForLog(trace, includeBody);
    }
    if (trace.kind == axtp::HidReportTraceKind::AcceptedReport) {
        return rxFrameForLog(trace, includeBody);
    }

    std::ostringstream out;
    out << "hid " << hidTraceDirectionName(trace.kind) << " " << hidTraceKindName(trace.kind);
    if (trace.timeoutMs > 0) {
        out << " timeoutMs=" << trace.timeoutMs;
    }
    if (trace.kind == axtp::HidReportTraceKind::ReadTimeout ||
        trace.kind == axtp::HidReportTraceKind::ReadError) {
        if (!trace.message.empty()) {
            out << " message=" << trace.message;
        }
        return out.str();
    }

    out << " size=" << trace.size << " reportId=" << toHexByte(trace.reportId)
        << " expectedReportId=" << toHexByte(trace.expectedReportId);
    if (!trace.message.empty()) {
        out << " message=" << trace.message;
    }
    if (includeBody && trace.data != nullptr && trace.size > 0) {
        const axtp::Bytes bytes(trace.data, trace.data + trace.size);
        out << " hex=" << toHex(bytes);
    }
    return out.str();
}

class LocalLogger {
public:
    bool open(const CliOptions& options, const char* argv0) {
        includeBody_ = options.logBody;
        if (!options.logEnabled && !options.logBody) {
            return false;
        }

        const auto exe = executablePath(argv0);
        const auto dir = exe.parent_path() / "axtpctl-logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cerr << "failed to create log directory: " << dir.string() << "\n";
            return false;
        }

        std::ostringstream name;
        name << "axtpctl-" << formatTime("%Y%m%d-%H%M%S") << "-" << processId() << ".log";
        path_ = dir / name.str();
        stream_.open(path_, std::ios::out | std::ios::app);
        if (!stream_) {
            std::cerr << "failed to open log file: " << path_.string() << "\n";
            return false;
        }

        write("log opened");
        write(std::string("exe=") + exe.string());
        write(std::string("body logging=") + (includeBody_ ? "enabled" : "disabled"));
        return true;
    }

    void write(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stream_) {
            return;
        }
        stream_ << formatTime("%Y-%m-%d %H:%M:%S") << " " << message << "\n";
        stream_.flush();
    }

    bool enabled() const {
        return stream_.is_open();
    }

    bool includeBody() const {
        return includeBody_;
    }

    std::string pathString() const {
        return path_.string();
    }

private:
    std::mutex mutex_;
    std::ofstream stream_;
    std::filesystem::path path_;
    bool includeBody_ = false;
};

class ReadOnlyByteSink final : public axtp::IByteSink {
public:
    void onBytes(const axtp::Byte*, std::size_t size) override {
        bytes.fetch_add(size);
        chunks.fetch_add(1);
    }

    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> chunks{0};
};

bool hasHidTarget(const CliOptions& options) {
    return !options.hidPath.empty() || (options.vid.has_value() && options.pid.has_value());
}

std::string hidTraceJsonForConsole(const axtp::HidReportTrace& trace) {
    std::ostringstream out;
    out << "{\"direction\":\"" << hidTraceDirectionName(trace.kind) << "\",\"kind\":\""
        << hidTraceKindName(trace.kind) << "\"";
    if (trace.timeoutMs > 0) {
        out << ",\"timeoutMs\":" << trace.timeoutMs;
    }
    out << ",\"size\":" << trace.size << ",\"reportId\":\"" << toHexByte(trace.reportId)
        << "\",\"expectedReportId\":\"" << toHexByte(trace.expectedReportId) << "\"";
    if (!trace.message.empty()) {
        out << ",\"message\":\"" << jsonEscape(trace.message) << "\"";
    }
    if (trace.data != nullptr && trace.size > 0) {
        const axtp::Bytes bytes(trace.data, trace.data + trace.size);
        out << ",\"hex\":\"" << toHex(bytes) << "\"";
    }
    out << "}";
    return out.str();
}

void printReadOnlyTrace(const axtp::HidReportTrace& trace,
                        OutputFormat format,
                        std::mutex* outputMutex) {
    if (std::string_view(hidTraceDirectionName(trace.kind)) != "rx") {
        return;
    }

    std::lock_guard<std::mutex> lock(*outputMutex);
    if (format == OutputFormat::Hex) {
        if (trace.kind == axtp::HidReportTraceKind::ReadReport && trace.data != nullptr &&
            trace.size > 0) {
            const axtp::Bytes bytes(trace.data, trace.data + trace.size);
            std::cout << toHex(bytes) << "\n";
        } else if (trace.kind == axtp::HidReportTraceKind::ReadError) {
            std::cerr << hidTraceForLog(trace, false) << "\n";
        }
        return;
    }

    if (format == OutputFormat::Json) {
        std::cout << hidTraceJsonForConsole(trace) << "\n";
        return;
    }

    if (trace.kind == axtp::HidReportTraceKind::ReadReport ||
        trace.kind == axtp::HidReportTraceKind::AcceptedReport ||
        trace.kind == axtp::HidReportTraceKind::DroppedReportId) {
        std::cout << hidTraceForLog(trace, true) << "\n";
        return;
    }
    std::cerr << hidTraceForLog(trace, false) << "\n";
}

void printCallTrace(const axtp::HidReportTrace& trace,
                    bool includeBody,
                    std::mutex* outputMutex) {
    const bool shouldPrint = trace.kind == axtp::HidReportTraceKind::WriteFrame ||
                             trace.kind == axtp::HidReportTraceKind::AcceptedReport ||
                             trace.kind == axtp::HidReportTraceKind::DroppedReportId ||
                             trace.kind == axtp::HidReportTraceKind::ReadError ||
                             trace.kind == axtp::HidReportTraceKind::WriteError;
    if (!shouldPrint) {
        return;
    }

    std::lock_guard<std::mutex> lock(*outputMutex);
    std::cerr << hidTraceForLog(trace, includeBody) << "\n";
}

std::string appReadyTraceForLog(const axtp::sdk::AppReadyTraceEvent& event, bool includeBody) {
    std::ostringstream out;
    out << "app-ready trace"
        << " stage=" << event.stage
        << " action=" << event.action
        << " status=" << errorName(event.statusCode) << "("
        << static_cast<std::uint16_t>(event.statusCode) << ")"
        << " randomSeed=" << toHexU32(event.randomSeed);
    if (event.controlId != 0) {
        out << " controlId=" << event.controlId;
    }
    if (!event.sid.empty()) {
        out << " sid=" << event.sid;
    }
    if (!event.detail.empty()) {
        out << " detail=" << event.detail;
    }
    if (includeBody && !event.bodyText.empty()) {
        out << " body=" << event.bodyText;
    }
    return out.str();
}

void printAppReadyTrace(const axtp::sdk::AppReadyTraceEvent& event,
                        bool includeBody,
                        std::mutex* outputMutex) {
    std::lock_guard<std::mutex> lock(*outputMutex);
    std::cerr << appReadyTraceForLog(event, includeBody) << "\n";
}

bool parseArgs(int argc, char** argv, CliOptions* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };
        auto parseNumberOption = [&](const char* name) -> std::optional<std::uint32_t> {
            const auto value = requireValue(name);
            if (!value.has_value()) {
                return std::nullopt;
            }
            const auto parsed = parseUint32(*value);
            if (!parsed.has_value()) {
                std::cerr << "invalid value for " << name << "\n";
                return std::nullopt;
            }
            return parsed;
        };

        if (arg == "-h" || arg == "--help") {
            options->help = true;
            continue;
        }
        if (arg == "--log") {
            options->logEnabled = true;
            continue;
        }
        if (arg == "--no-log") {
            options->logEnabled = false;
            options->logBody = false;
            continue;
        }
        if (arg == "--log-body") {
            options->logEnabled = true;
            options->logBody = true;
            continue;
        }
        if (arg == "--no-app-ready") {
            options->noAppReady = true;
            continue;
        }
        if (arg == "-c" || arg == "--command") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value() || value->empty()) {
                std::cerr << "method name must not be empty\n";
                return false;
            }
            options->commandMethod = *value;
            continue;
        }
        if (arg == "-t" || arg == "--transport") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->transport = *value;
            continue;
        }
        if (arg == "-j" || arg == "--json") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->json = *value;
            continue;
        }
        if (arg == "-f" || arg == "--json-file") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->jsonFile = *value;
            continue;
        }
        if (arg == "-o" || arg == "--output") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->output = *value;
            continue;
        }
        if (arg == "--path" || arg == "--hid-path") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->hidPath = *value;
            continue;
        }
        if (arg == "--serial") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->serialNumber = *value;
            continue;
        }
        if (arg == "--sid") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->sid = *value;
            continue;
        }
        if (arg == "--random-seed") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->randomSeed = *value;
            continue;
        }
        if (arg == "--raw-hex") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->rawHex = *value;
            continue;
        }
        if (arg == "--raw-file") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->rawFile = *value;
            continue;
        }
        if (arg == "--tlv-hex") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->tlvHex = *value;
            continue;
        }
        if (arg == "--tlv-file") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->tlvFile = *value;
            continue;
        }
        if (arg == "--method-id") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value() || *value > 0xFFFF) {
                std::cerr << "invalid --method-id\n";
                return false;
            }
            options->methodId = *value;
            continue;
        }
        if (arg == "--vid") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value() || *value > 0xFFFF) {
                std::cerr << "invalid --vid\n";
                return false;
            }
            options->vid = *value;
            continue;
        }
        if (arg == "--pid") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value() || *value > 0xFFFF) {
                std::cerr << "invalid --pid\n";
                return false;
            }
            options->pid = *value;
            continue;
        }
        if (arg == "--timeout") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->timeoutMs = *value;
            continue;
        }
        if (arg == "--report-id") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value() || *value > 0xFF) {
                std::cerr << "invalid --report-id\n";
                return false;
            }
            options->reportId = *value;
            continue;
        }
        if (arg == "--input-report-size") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value() || *value == 0) {
                std::cerr << "invalid --input-report-size\n";
                return false;
            }
            options->inputReportSize = *value;
            continue;
        }
        if (arg == "--read-buffer-size") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value() || *value == 0) {
                std::cerr << "invalid --read-buffer-size\n";
                return false;
            }
            options->readBufferSize = *value;
            continue;
        }
        if (arg == "--output-report-size") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value() || *value == 0) {
                std::cerr << "invalid --output-report-size\n";
                return false;
            }
            options->outputReportSize = *value;
            continue;
        }
        if (arg == "--max-reports-per-poll") {
            const auto value = parseNumberOption(arg.c_str());
            if (!value.has_value() || *value == 0) {
                std::cerr << "invalid --max-reports-per-poll\n";
                return false;
            }
            options->maxReportsPerPoll = *value;
            continue;
        }
        if (isOption(arg)) {
            std::cerr << "unknown option: " << arg << "\n";
            return false;
        }
        options->positional.push_back(arg);
    }

    if (options->commandMethod.has_value() && !options->positional.empty()) {
        std::cerr << "-c/--command cannot be combined with an explicit command\n";
        return false;
    }
    return true;
}

int printMethods(OutputFormat format) {
    if (format == OutputFormat::Json) {
        std::cout << "[";
        for (std::size_t i = 0; i < axtp::kMethodRegistryCount; ++i) {
            const auto& method = axtp::kMethodRegistry[i];
            if (i != 0) {
                std::cout << ",";
            }
            std::cout << "{\"id\":" << method.id << ",\"idHex\":\"" << toHexId(method.id)
                      << "\",\"name\":\"" << jsonEscape(method.name) << "\",\"domain\":\""
                      << jsonEscape(method.domain) << "\",\"requestSchema\":\""
                      << jsonEscape(method.request_schema) << "\",\"responseSchema\":\""
                      << jsonEscape(method.response_schema) << "\"}";
        }
        std::cout << "]\n";
        return 0;
    }

    for (const auto& method : axtp::kMethodRegistry) {
        std::cout << toHexId(method.id) << "  " << method.name << "  " << method.request_schema
                  << " -> " << method.response_schema << "\n";
    }
    return 0;
}

int printHidDevices(const CliOptions& options, OutputFormat format) {
    const auto vid = static_cast<std::uint16_t>(options.vid.value_or(0));
    const auto pid = static_cast<std::uint16_t>(options.pid.value_or(0));
    const auto devices = axtp::enumerateHidDevices(vid, pid);

    if (format == OutputFormat::Json) {
        std::cout << "[";
        for (std::size_t index = 0; index < devices.size(); ++index) {
            const auto& device = devices[index];
            if (index != 0) {
                std::cout << ",";
            }
            std::cout << "{\"index\":" << index << ",\"vid\":\"" << toHexId(device.vendorId)
                      << "\",\"pid\":\"" << toHexId(device.productId) << "\",\"path\":\""
                      << jsonEscape(device.path) << "\",\"serial\":\""
                      << jsonEscape(device.serialNumber) << "\",\"manufacturer\":\""
                      << jsonEscape(device.manufacturer) << "\",\"product\":\""
                      << jsonEscape(device.product) << "\",\"usagePage\":\""
                      << toHexId(device.usagePage) << "\",\"usage\":\"" << toHexId(device.usage)
                      << "\",\"interfaceNumber\":" << device.interfaceNumber << ",\"busType\":\""
                      << jsonEscape(device.busType) << "\"}";
        }
        std::cout << "]\n";
        return 0;
    }

    if (devices.empty()) {
        std::cout << "No HID devices found";
        if (options.vid.has_value() || options.pid.has_value()) {
            std::cout << " for vid=" << toHexId(options.vid.value_or(0))
                      << " pid=" << toHexId(options.pid.value_or(0));
        }
        std::cout << "\n";
        return 0;
    }

    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        std::cout << "[" << index << "] vid=" << toHexId(device.vendorId)
                  << " pid=" << toHexId(device.productId) << " bus=" << device.busType
                  << " usagePage=" << toHexId(device.usagePage)
                  << " usage=" << toHexId(device.usage)
                  << " interface=" << device.interfaceNumber << "\n";
        if (!device.manufacturer.empty() || !device.product.empty()) {
            std::cout << "    name=" << device.manufacturer << " " << device.product << "\n";
        }
        if (!device.serialNumber.empty()) {
            std::cout << "    serial=" << device.serialNumber << "\n";
        }
        std::cout << "    path=" << device.path << "\n";
    }
    return 0;
}

bool buildRequestBody(const CliOptions& options,
                      axtp::RpcEncoding* encoding,
                      axtp::Bytes* body,
                      LocalLogger* logger) {
    const int bodySourceCount = static_cast<int>(options.json.has_value()) +
                                static_cast<int>(options.jsonFile.has_value()) +
                                static_cast<int>(options.rawHex.has_value()) +
                                static_cast<int>(options.rawFile.has_value()) +
                                static_cast<int>(options.tlvHex.has_value()) +
                                static_cast<int>(options.tlvFile.has_value());
    if (bodySourceCount > 1) {
        std::cerr << "choose only one of --json, --json-file, --raw-hex, --raw-file, --tlv-hex, "
                     "or --tlv-file\n";
        if (logger != nullptr) {
            logger->write("request body error: multiple body sources");
        }
        return false;
    }

    *encoding = axtp::RpcEncoding::Json;
    body->assign({'{', '}'});

    if (options.json.has_value()) {
        body->assign(options.json->begin(), options.json->end());
        return true;
    }
    if (options.jsonFile.has_value()) {
        const auto text = readTextFile(*options.jsonFile);
        if (!text.has_value()) {
            std::cerr << "failed to read JSON file: " << *options.jsonFile << "\n";
            if (logger != nullptr) {
                logger->write(std::string("request body error: failed to read JSON file ") +
                              *options.jsonFile);
            }
            return false;
        }
        body->assign(text->begin(), text->end());
        return true;
    }
    if (options.rawHex.has_value() || options.tlvHex.has_value()) {
        const auto& source = options.rawHex.has_value() ? *options.rawHex : *options.tlvHex;
        const auto bytes = parseHex(source);
        if (!bytes.has_value()) {
            std::cerr << "invalid hex body\n";
            if (logger != nullptr) {
                logger->write("request body error: invalid hex body");
            }
            return false;
        }
        *encoding = axtp::jsonBinaryRpcEncoding();
        *body = *bytes;
        return true;
    }
    if (options.rawFile.has_value() || options.tlvFile.has_value()) {
        const auto& source = options.rawFile.has_value() ? *options.rawFile : *options.tlvFile;
        const auto bytes = readBinaryFile(source);
        if (!bytes.has_value()) {
            std::cerr << "failed to read binary body file: " << source << "\n";
            if (logger != nullptr) {
                logger->write(std::string("request body error: failed to read binary file ") +
                              source);
            }
            return false;
        }
        *encoding = axtp::jsonBinaryRpcEncoding();
        *body = *bytes;
        return true;
    }

    return true;
}

std::optional<std::string> commandName(const CliOptions& options) {
    if (options.help) {
        return std::string("help");
    }
    if (options.commandMethod.has_value()) {
        return std::string("call");
    }
    if (options.positional.empty()) {
        return std::string("help");
    }
    return options.positional.front();
}

std::optional<std::string> methodNameFromCommand(const CliOptions& options) {
    if (options.commandMethod.has_value()) {
        return options.commandMethod;
    }
    if (options.positional.size() >= 2 && options.positional[0] == "call" &&
        !isOption(options.positional[1])) {
        return options.positional[1];
    }
    if (options.positional.size() == 1 && options.positional[0].find('.') != std::string::npos) {
        return options.positional[0];
    }
    return std::nullopt;
}

int readHid(const CliOptions& options, LocalLogger& logger) {
    if (options.transport != "hid" && options.transport != "hidapi") {
        std::cerr << "this axtpctl build supports HID only; unsupported transport: "
                  << options.transport << "\n";
        logger.write(std::string("unsupported transport: ") + options.transport);
        return 2;
    }
    if (!hasHidTarget(options)) {
        std::cerr << "read-hid requires --path/--hid-path or both --vid and --pid\n";
        logger.write("read-hid error: missing --path or --vid/--pid");
        return 2;
    }

    std::mutex outputMutex;
    const auto format = parseOutputFormat(options.output);

    axtp::HidTransportOptions hidOptions;
    hidOptions.vendorId = static_cast<std::uint16_t>(options.vid.value_or(0));
    hidOptions.productId = static_cast<std::uint16_t>(options.pid.value_or(0));
    hidOptions.devicePath = options.hidPath;
    hidOptions.serialNumber = options.serialNumber;
    hidOptions.reportId = static_cast<std::uint8_t>(options.reportId);
    hidOptions.inputReportSize = static_cast<std::size_t>(options.inputReportSize);
    hidOptions.readBufferSize = static_cast<std::size_t>(options.readBufferSize);
    hidOptions.outputReportSize = static_cast<std::size_t>(options.outputReportSize);
    hidOptions.maxReportsPerPoll = static_cast<std::size_t>(options.maxReportsPerPoll);
    hidOptions.useReadThread = true;
    hidOptions.readThreadTimeoutMs = 1000;
    hidOptions.reportTrace = [&logger, format, &outputMutex](const axtp::HidReportTrace& trace) {
        logger.write(hidTraceForLog(trace, logger.includeBody()));
        printReadOnlyTrace(trace, format, &outputMutex);
    };

    std::ostringstream openLog;
    openLog << "opening HID read-only vid=" << toHexId(options.vid.value_or(0))
            << " pid=" << toHexId(options.pid.value_or(0))
            << " path=" << (options.hidPath.empty() ? "<none>" : options.hidPath)
            << " serial=" << (options.serialNumber.empty() ? "<none>" : options.serialNumber)
            << " backend=hidapi"
            << " openMode=" << (options.hidPath.empty() ? "vid-pid" : "path")
            << " tx=disabled"
            << " reportId=" << toHexByte(options.reportId)
            << " inputReportSize=" << options.inputReportSize
            << " readBufferSize=" << hidOptions.readBufferSize
            << " outputReportSize=" << options.outputReportSize
            << " maxReportsPerPoll=" << options.maxReportsPerPoll
            << " rxThread=enabled"
            << " readTimeoutMs=" << hidOptions.readThreadTimeoutMs
            << " durationMs=" << options.timeoutMs;
    logger.write(openLog.str());

    auto transport = std::make_unique<axtp::HidTransport>(std::move(hidOptions));
    ReadOnlyByteSink sink;
    transport->bind(sink);
    transport->open();
    if (!transport->isOpen()) {
        std::cerr << "failed to open HID device";
        if (options.vid.has_value() || options.pid.has_value()) {
            std::cerr << " vid=" << toHexId(options.vid.value_or(0))
                      << " pid=" << toHexId(options.pid.value_or(0));
        }
        if (!options.hidPath.empty()) {
            std::cerr << " path=" << options.hidPath;
        }
        if (!options.serialNumber.empty()) {
            std::cerr << " serial=" << options.serialNumber;
        }
        std::cerr << "\n";
        logger.write("failed to open HID device for read-hid");
        return 4;
    }

    logger.write("HID device opened read-only");
    std::cerr << "read-hid opened; sending is disabled; ";
    if (options.timeoutMs == 0) {
        std::cerr << "reading until Ctrl+C\n";
    } else {
        std::cerr << "reading for " << options.timeoutMs << " ms\n";
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(options.timeoutMs);
    while (options.timeoutMs == 0 || std::chrono::steady_clock::now() < deadline) {
        transport->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto hidStats = transport->stats();
    transport->close();

    std::ostringstream hidStatsLog;
    hidStatsLog << "hid read-only stats readReports=" << hidStats.readReports
                << " readBytes=" << hidStats.readBytes
                << " acceptedReports=" << hidStats.acceptedReports
                << " droppedReportId=" << hidStats.droppedReportId
                << " readErrors=" << hidStats.readErrors
                << " queuedReports=" << hidStats.queuedReports
                << " writeReports=" << hidStats.writeReports
                << " writeBytes=" << hidStats.writeBytes
                << " writeErrors=" << hidStats.writeErrors
                << " sinkChunks=" << sink.chunks.load()
                << " sinkBytes=" << sink.bytes.load();
    logger.write(hidStatsLog.str());

    std::cerr << hidStatsLog.str() << "\n";
    return hidStats.readErrors == 0 ? 0 : 4;
}

int handshakeHid(const CliOptions& options, LocalLogger& logger) {
    if (options.transport != "hid" && options.transport != "hidapi") {
        std::cerr << "this axtpctl build supports HID only; unsupported transport: "
                  << options.transport << "\n";
        logger.write(std::string("unsupported transport: ") + options.transport);
        return 2;
    }
    if (!hasHidTarget(options)) {
        std::cerr << "handshake requires --path/--hid-path or both --vid and --pid\n";
        logger.write("handshake error: missing --path or --vid/--pid");
        return 2;
    }

    std::mutex outputMutex;
    axtp::HidTransportOptions hidOptions;
    hidOptions.vendorId = static_cast<std::uint16_t>(options.vid.value_or(0));
    hidOptions.productId = static_cast<std::uint16_t>(options.pid.value_or(0));
    hidOptions.devicePath = options.hidPath;
    hidOptions.serialNumber = options.serialNumber;
    hidOptions.reportId = static_cast<std::uint8_t>(options.reportId);
    hidOptions.inputReportSize = static_cast<std::size_t>(options.inputReportSize);
    hidOptions.readBufferSize = static_cast<std::size_t>(options.readBufferSize);
    hidOptions.outputReportSize = static_cast<std::size_t>(options.outputReportSize);
    hidOptions.maxReportsPerPoll = static_cast<std::size_t>(options.maxReportsPerPoll);
    hidOptions.useReadThread = true;
    hidOptions.readThreadTimeoutMs = 1000;
    hidOptions.reportTrace = [&logger, &outputMutex](const axtp::HidReportTrace& trace) {
        logger.write(hidTraceForLog(trace, logger.includeBody()));
        printCallTrace(trace, logger.includeBody(), &outputMutex);
    };

    std::ostringstream openLog;
    openLog << "opening HID handshake vid=" << toHexId(options.vid.value_or(0))
            << " pid=" << toHexId(options.pid.value_or(0))
            << " path=" << (options.hidPath.empty() ? "<none>" : options.hidPath)
            << " serial=" << (options.serialNumber.empty() ? "<none>" : options.serialNumber)
            << " backend=hidapi"
            << " openMode=" << (options.hidPath.empty() ? "vid-pid" : "path")
            << " reportId=" << toHexByte(options.reportId)
            << " inputReportSize=" << options.inputReportSize
            << " readBufferSize=" << hidOptions.readBufferSize
            << " outputReportSize=" << options.outputReportSize
            << " maxReportsPerPoll=" << options.maxReportsPerPoll
            << " rxThread=enabled"
            << " readTimeoutMs=" << hidOptions.readThreadTimeoutMs
            << " timeoutMs=" << options.timeoutMs;
    logger.write(openLog.str());

    auto transport = std::make_unique<axtp::HidTransport>(std::move(hidOptions));
    auto* hidTransport = transport.get();

    axtp::sdk::ClientOptions clientOptions;
    clientOptions.autoIdentify = !options.noAppReady;
    axtp::sdk::AxtpClient client(clientOptions);
    client.attachTransport(std::move(transport));
    if (hidTransport == nullptr || !hidTransport->isOpen()) {
        std::cerr << "failed to open HID device";
        if (options.vid.has_value() || options.pid.has_value()) {
            std::cerr << " vid=" << toHexId(options.vid.value_or(0))
                      << " pid=" << toHexId(options.pid.value_or(0));
        }
        if (!options.hidPath.empty()) {
            std::cerr << " path=" << options.hidPath;
        }
        if (!options.serialNumber.empty()) {
            std::cerr << " serial=" << options.serialNumber;
        }
        std::cerr << "\n";
        logger.write("failed to open HID device for handshake");
        return 4;
    }
    logger.write("HID device opened for handshake");

    axtp::sdk::AppReadyOptions appOptions;
    appOptions.timeout = std::chrono::milliseconds(options.timeoutMs);
    appOptions.randomSeed = options.randomSeed;
    appOptions.trace = [&logger, &outputMutex](const axtp::sdk::AppReadyTraceEvent& event) {
        logger.write(appReadyTraceForLog(event, true));
        printAppReadyTrace(event, true, &outputMutex);
    };

    const auto started = std::chrono::steady_clock::now();
    const auto result = client.ensureAppReady(appOptions);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();

    std::ostringstream appLog;
    appLog << "app-ready ok=" << (result.ok ? "true" : "false")
           << " stage=" << result.stage
           << " status=" << errorName(result.statusCode) << "("
           << static_cast<std::uint16_t>(result.statusCode) << ")"
           << " sid=" << (result.sid.empty() ? "<none>" : result.sid)
           << " randomSeed=" << toHexU32(result.randomSeed)
           << " elapsedMs=" << elapsedMs;
    logger.write(appLog.str());

    const auto hidStats = hidTransport != nullptr ? hidTransport->stats() : axtp::HidTransportStats{};
    client.close();

    std::ostringstream hidStatsLog;
    hidStatsLog << "hid handshake stats readReports=" << hidStats.readReports
                << " readBytes=" << hidStats.readBytes
                << " acceptedReports=" << hidStats.acceptedReports
                << " droppedReportId=" << hidStats.droppedReportId
                << " readErrors=" << hidStats.readErrors
                << " queuedReports=" << hidStats.queuedReports
                << " writeReports=" << hidStats.writeReports
                << " writeBytes=" << hidStats.writeBytes
                << " writeErrors=" << hidStats.writeErrors;
    logger.write(hidStatsLog.str());

    const auto format = parseOutputFormat(options.output);
    if (format == OutputFormat::Json) {
        std::cout << "{\"ok\":" << (result.ok ? "true" : "false")
                  << ",\"stage\":\"" << jsonEscape(result.stage)
                  << "\",\"statusCode\":" << static_cast<std::uint16_t>(result.statusCode)
                  << ",\"status\":\"" << jsonEscape(errorName(result.statusCode))
                  << "\",\"sid\":\"" << jsonEscape(result.sid)
                  << "\",\"randomSeed\":" << result.randomSeed
                  << ",\"randomSeedHex\":\"" << toHexU32(result.randomSeed)
                  << "\",\"elapsedMs\":" << elapsedMs << "}\n";
        return result.ok ? 0 : 4;
    }

    if (!result.ok) {
        std::cerr << "AXTP app-ready failed at " << result.stage << ": "
                  << errorName(result.statusCode) << " ("
                  << static_cast<std::uint16_t>(result.statusCode) << ")\n";
        return 4;
    }

    std::cout << "APP_READY sid=" << result.sid
              << " randomSeed=" << toHexU32(result.randomSeed)
              << " elapsedMs=" << elapsedMs << "\n";
    return 0;
}

int callMethod(const CliOptions& options, LocalLogger& logger) {
    if (options.transport != "hid" && options.transport != "hidapi") {
        std::cerr << "this axtpctl build supports HID only; unsupported transport: "
                  << options.transport << "\n";
        logger.write(std::string("unsupported transport: ") + options.transport);
        return 2;
    }

    auto methodId = options.methodId;
    auto methodName = methodNameFromCommand(options);
    if (!methodId.has_value() && methodName.has_value()) {
        methodId = axtp::RegistryLookup::methodIdByName(*methodName);
    }
    if (!methodId.has_value()) {
        if (methodName.has_value()) {
            std::cerr << "unknown method: " << *methodName << "\n";
            logger.write(std::string("unknown method: ") + *methodName);
        } else {
            std::cerr << "call requires <method> or --method-id\n";
            logger.write("call error: missing method name or method id");
        }
        return 2;
    }
    if (!methodName.has_value()) {
        if (const auto* descriptor =
                axtp::RegistryLookup::methodById(static_cast<std::uint16_t>(*methodId))) {
            methodName = descriptor->name;
        }
    }

    axtp::RpcEncoding encoding = axtp::RpcEncoding::Json;
    axtp::Bytes body;
    if (!buildRequestBody(options, &encoding, &body, &logger)) {
        return 2;
    }
    if (options.hidPath.empty() && (!options.vid.has_value() || !options.pid.has_value())) {
        std::cerr << "HID calls require --path/--hid-path or both --vid and --pid\n";
        logger.write("call error: missing --path or --vid/--pid");
        return 2;
    }

    std::ostringstream requestLog;
    requestLog << "call method=" << (methodName.has_value() ? *methodName : "<unknown>")
               << " methodId=" << toHexId(*methodId) << " encoding=" << encodingName(encoding)
               << " timeoutMs=" << options.timeoutMs
               << " acceptAnyResponse=true"
               << " appReady=" << (options.noAppReady ? "disabled" : "enabled")
               << " sid="
               << (encoding == axtp::RpcEncoding::Json
                       ? (options.noAppReady ? options.sid : "<app-ready>")
                       : "<binary>")
               << " randomSeed="
               << (options.randomSeed.has_value() ? toHexU32(*options.randomSeed) : "<auto>")
               << " body=" << bytesForLog(body, encoding, logger.includeBody());
    logger.write(requestLog.str());

    std::mutex outputMutex;
    axtp::HidTransportOptions hidOptions;
    hidOptions.vendorId = static_cast<std::uint16_t>(options.vid.value_or(0));
    hidOptions.productId = static_cast<std::uint16_t>(options.pid.value_or(0));
    hidOptions.devicePath = options.hidPath;
    hidOptions.serialNumber = options.serialNumber;
    hidOptions.reportId = static_cast<std::uint8_t>(options.reportId);
    hidOptions.inputReportSize = static_cast<std::size_t>(options.inputReportSize);
    hidOptions.readBufferSize = static_cast<std::size_t>(options.readBufferSize);
    hidOptions.outputReportSize = static_cast<std::size_t>(options.outputReportSize);
    hidOptions.maxReportsPerPoll = static_cast<std::size_t>(options.maxReportsPerPoll);
    hidOptions.useReadThread = true;
    hidOptions.readThreadTimeoutMs = 1000;
    hidOptions.reportTrace = [&logger, &outputMutex](const axtp::HidReportTrace& trace) {
        logger.write(hidTraceForLog(trace, logger.includeBody()));
        printCallTrace(trace, logger.includeBody(), &outputMutex);
    };

    std::ostringstream openLog;
    openLog << "opening HID vid=" << toHexId(options.vid.value_or(0))
            << " pid=" << toHexId(options.pid.value_or(0))
            << " path=" << (options.hidPath.empty() ? "<none>" : options.hidPath)
            << " serial=" << (options.serialNumber.empty() ? "<none>" : options.serialNumber)
            << " backend=hidapi"
            << " openMode=" << (options.hidPath.empty() ? "vid-pid" : "path")
            << " reportId=" << toHexByte(options.reportId)
            << " inputReportSize=" << options.inputReportSize
            << " readBufferSize=" << hidOptions.readBufferSize
            << " outputReportSize=" << options.outputReportSize
            << " maxReportsPerPoll=" << options.maxReportsPerPoll
            << " rxThread=enabled"
            << " readTimeoutMs=" << hidOptions.readThreadTimeoutMs;
    logger.write(openLog.str());

    auto transport = std::make_unique<axtp::HidTransport>(std::move(hidOptions));
    auto* hidTransport = transport.get();

    axtp::sdk::ClientOptions clientOptions;
    clientOptions.autoIdentify = !options.noAppReady;
    axtp::sdk::AxtpClient client(clientOptions);
    client.attachTransport(std::move(transport));
    if (hidTransport == nullptr || !hidTransport->isOpen()) {
        std::cerr << "failed to open HID device";
        if (options.vid.has_value() || options.pid.has_value()) {
            std::cerr << " vid=" << toHexId(options.vid.value_or(0))
                      << " pid=" << toHexId(options.pid.value_or(0));
        }
        if (!options.hidPath.empty()) {
            std::cerr << " path=" << options.hidPath;
        }
        if (!options.serialNumber.empty()) {
            std::cerr << " serial=" << options.serialNumber;
        }
        std::cerr << "\n";
        logger.write("failed to open HID device");
        return 4;
    }
    logger.write("HID device opened");

    std::string effectiveSid = options.sid;
    axtp::sdk::AppReadyResult appReadyResult;
    if (!options.noAppReady) {
        axtp::sdk::AppReadyOptions appOptions;
        appOptions.timeout = std::chrono::milliseconds(options.timeoutMs);
        appOptions.randomSeed = options.randomSeed;
        appOptions.trace = [&logger, &outputMutex](const axtp::sdk::AppReadyTraceEvent& event) {
            logger.write(appReadyTraceForLog(event, true));
            printAppReadyTrace(event, true, &outputMutex);
        };

        const auto started = std::chrono::steady_clock::now();
        appReadyResult = client.ensureAppReady(appOptions);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();

        std::ostringstream appLog;
        appLog << "app-ready ok=" << (appReadyResult.ok ? "true" : "false")
               << " stage=" << appReadyResult.stage
               << " status=" << errorName(appReadyResult.statusCode) << "("
               << static_cast<std::uint16_t>(appReadyResult.statusCode) << ")"
               << " sid=" << (appReadyResult.sid.empty() ? "<none>" : appReadyResult.sid)
               << " randomSeed=" << toHexU32(appReadyResult.randomSeed)
               << " elapsedMs=" << elapsedMs;
        logger.write(appLog.str());

        if (!appReadyResult.ok) {
            const auto hidStats =
                hidTransport != nullptr ? hidTransport->stats() : axtp::HidTransportStats{};
            client.close();

            std::ostringstream hidStatsLog;
            hidStatsLog << "hid stats readReports=" << hidStats.readReports
                        << " readBytes=" << hidStats.readBytes
                        << " acceptedReports=" << hidStats.acceptedReports
                        << " droppedReportId=" << hidStats.droppedReportId
                        << " readErrors=" << hidStats.readErrors
                        << " queuedReports=" << hidStats.queuedReports
                        << " writeReports=" << hidStats.writeReports
                        << " writeBytes=" << hidStats.writeBytes
                        << " writeErrors=" << hidStats.writeErrors;
            logger.write(hidStatsLog.str());

            const auto format = parseOutputFormat(options.output);
            if (format == OutputFormat::Json) {
                std::cout << "{\"ok\":false";
                if (methodName.has_value()) {
                    std::cout << ",\"method\":\"" << jsonEscape(*methodName) << "\"";
                }
                std::cout << ",\"methodId\":" << *methodId
                          << ",\"methodIdHex\":\"" << toHexId(*methodId)
                          << "\",\"stage\":\"" << jsonEscape(appReadyResult.stage)
                          << "\",\"statusCode\":"
                          << static_cast<std::uint16_t>(appReadyResult.statusCode)
                          << ",\"status\":\"" << jsonEscape(errorName(appReadyResult.statusCode))
                          << "\",\"sid\":\"" << jsonEscape(appReadyResult.sid)
                          << "\",\"randomSeed\":" << appReadyResult.randomSeed
                          << ",\"randomSeedHex\":\"" << toHexU32(appReadyResult.randomSeed)
                          << "\"}\n";
                return 4;
            }

            std::cerr << "AXTP app-ready failed at " << appReadyResult.stage << ": "
                      << errorName(appReadyResult.statusCode) << " ("
                      << static_cast<std::uint16_t>(appReadyResult.statusCode) << ")\n";
            return 4;
        }
        effectiveSid = appReadyResult.sid;
    }

    axtp::RpcPayload request;
    request.encoding = encoding;
    request.op = axtp::RpcOp::Request;
    request.methodOrEventId = *methodId;
    request.bodyEncoding = axtp::bodyEncodingForRpcEncoding(encoding);
    if (encoding == axtp::RpcEncoding::Json) {
        request.bodyEncoding = axtp::RpcBodyEncoding::None;
        request.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
        request.meta.jsonSid = effectiveSid;
    } else {
        request.meta.sourceProtocol = axtp::SourceProtocol::AxtpV1;
    }
    if (methodName.has_value()) {
        request.meta.jsonMethodOrEventName = *methodName;
    }
    request.body = std::move(body);

    axtp::sdk::CallOptions callOptions;
    callOptions.timeout = std::chrono::milliseconds(options.timeoutMs);
    callOptions.encoding = encoding;
    callOptions.acceptAnyResponse = true;
    auto response = client.callRaw(std::move(request), callOptions);
    const auto hidStats = hidTransport != nullptr ? hidTransport->stats() : axtp::HidTransportStats{};
    client.close();

    std::ostringstream hidStatsLog;
    hidStatsLog << "hid stats readReports=" << hidStats.readReports
                << " readBytes=" << hidStats.readBytes
                << " acceptedReports=" << hidStats.acceptedReports
                << " droppedReportId=" << hidStats.droppedReportId
                << " readErrors=" << hidStats.readErrors
                << " queuedReports=" << hidStats.queuedReports
                << " writeReports=" << hidStats.writeReports
                << " writeBytes=" << hidStats.writeBytes
                << " writeErrors=" << hidStats.writeErrors;
    logger.write(hidStatsLog.str());

    std::ostringstream responseLog;
    responseLog << "response status=" << errorName(response.statusCode) << "("
                << static_cast<std::uint16_t>(response.statusCode) << ")"
                << " requestId=" << response.requestId
                << " encoding=" << encodingName(response.encoding)
                << " body=" << bytesForLog(response.body, response.encoding, logger.includeBody());
    logger.write(responseLog.str());

    const auto format = parseOutputFormat(options.output);
    const bool ok = response.statusCode == axtp::ErrorCode::Success;
    if (format == OutputFormat::Hex) {
        std::cout << toHex(response.body) << "\n";
        return ok ? 0 : 4;
    }
    if (format == OutputFormat::Json) {
        const std::string bodyText(response.body.begin(), response.body.end());
        std::cout << "{\"ok\":" << (ok ? "true" : "false");
        if (methodName.has_value()) {
            std::cout << ",\"method\":\"" << jsonEscape(*methodName) << "\"";
        }
        std::cout << ",\"methodId\":" << *methodId << ",\"methodIdHex\":\"" << toHexId(*methodId)
                  << "\",\"requestId\":" << response.requestId << ",\"statusCode\":"
                  << static_cast<std::uint16_t>(response.statusCode) << ",\"status\":\""
                  << jsonEscape(errorName(response.statusCode)) << "\",\"encoding\":\""
                  << encodingName(response.encoding) << "\",\"sid\":\""
                  << jsonEscape(effectiveSid) << "\"";
        if (!options.noAppReady) {
            std::cout << ",\"randomSeed\":" << appReadyResult.randomSeed
                      << ",\"randomSeedHex\":\"" << toHexU32(appReadyResult.randomSeed) << "\"";
        }
        std::cout << ",\"bodyText\":\""
                  << jsonEscape(bodyText) << "\",\"bodyHex\":\"" << toHex(response.body)
                  << "\"}\n";
        return ok ? 0 : 4;
    }

    if (!ok) {
        std::cerr << "AXTP call failed: " << errorName(response.statusCode) << " ("
                  << static_cast<std::uint16_t>(response.statusCode) << ")\n";
        return 4;
    }
    if (response.body.empty()) {
        std::cout << "OK\n";
    } else if (isMostlyText(response.body)) {
        std::cout << std::string(response.body.begin(), response.body.end()) << "\n";
    } else {
        std::cout << toHex(response.body) << "\n";
    }
    return 0;
}

int run(const CliOptions& options, LocalLogger& logger) {
    const auto command = commandName(options);
    const auto format = parseOutputFormat(options.output);
    if (!command.has_value() || *command == "help") {
        printUsage();
        return 0;
    }
    if (*command == "list-methods") {
        return printMethods(format);
    }
    if (*command == "list-hid") {
        return printHidDevices(options, format);
    }
    if (*command == "handshake") {
        return handshakeHid(options, logger);
    }
    if (*command == "read-hid") {
        return readHid(options, logger);
    }
    if (*command == "capability" && options.positional.size() >= 2 &&
        options.positional[1] == "methods") {
        return printMethods(format);
    }
    if (*command == "call" || (options.positional.size() == 1 &&
                               options.positional[0].find('.') != std::string::npos)) {
        return callMethod(options, logger);
    }

    std::cerr << "unknown command: " << *command << "\n";
    logger.write(std::string("unknown command: ") + *command);
    printUsage();
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    CliOptions options;
    try {
        if (!parseArgs(argc, argv, &options)) {
            return 2;
        }
        LocalLogger logger;
        logger.open(options, argc > 0 ? argv[0] : nullptr);
        if (logger.enabled()) {
            std::cerr << "axtpctl log: " << logger.pathString() << "\n";
            logger.write(std::string("argv=") + sanitizedArgv(argc, argv, logger.includeBody()));
        }
        const auto exitCode = run(options, logger);
        logger.write(std::string("exit code=") + std::to_string(exitCode));
        return exitCode;
    } catch (const std::exception& ex) {
        std::cerr << "axtpctl: " << ex.what() << "\n";
        return 1;
    }
}
