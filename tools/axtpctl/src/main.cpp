#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <websocketpp/common/md5.hpp>

#include "axtp_core.hpp"
#include "json_rpc/method_registry_json.hpp"
#include "profiles/firmware/firmware_profile.hpp"
#include "axtp_sdk.hpp"
#include "toolkit/axtp_toolkit.hpp"

namespace {

using axtp::toolkit::parseHex;
using axtp::toolkit::parseUint32;
using axtp::toolkit::toHex;

constexpr std::uint32_t kDefaultHidVendorId = 0x0581;
constexpr std::uint32_t kDefaultHidProductId = 0x2581;
constexpr std::uint32_t kDefaultHidUsagePage = 0x81;

enum class OutputFormat {
    Pretty,
    Json,
    Hex,
    File,
};

struct CliOptions {
    std::string transport = "mock";
    std::string executablePath;
    std::string endpoint;
    std::string host = "127.0.0.1";
    std::string path;
    std::string serialNumber;
    std::string wire = "websocket-json-rpc";
    std::string encoding = "json";
    std::string registryFile;
    std::string output = "pretty";
    std::string sid = "00000000";
    std::optional<std::string> shortcutMethod;
    std::optional<std::string> json;
    std::optional<std::string> jsonFile;
    std::optional<std::uint32_t> randomSeed;
    std::optional<std::uint32_t> port;
    std::optional<std::uint32_t> vid = kDefaultHidVendorId;
    std::optional<std::uint32_t> pid = kDefaultHidProductId;
    std::optional<std::uint32_t> usagePage = kDefaultHidUsagePage;
    std::optional<std::uint32_t> usage;
    std::uint32_t timeoutMs = 5000;
    std::uint32_t reportId = 0x05;
    std::uint32_t inputReportSize = 255;
    std::uint32_t readBufferSize = 4096;
    std::uint32_t outputReportSize = 255;
    std::uint32_t maxReportsPerPoll = 16;
    bool noAppReady = false;
    bool logEnabled = false;
    bool logBody = false;
    bool verbose = false;
    std::vector<std::string> command;
};

void printUsage() {
    std::cout << "Usage: axtpctl [options] <command>\n"
              << "       axtpctl -c <method> [--json JSON|--json-file FILE]\n"
              << "\n"
              << "Options:\n"
              << "  -c, --command <method>       Call an AXTP method by name\n"
              << "  -j, --json <json>            JSON params for the method call\n"
              << "  -f, --json-file <path>       Read JSON params from file\n"
              << "  -t, --transport <kind>       Select transport: hid, tcp, websocket, mock\n"
              << "  -o, --output <format>        Output format: pretty, json\n"
              << "      --host <host>            TCP host\n"
              << "      --port <port>            TCP or WebSocket port\n"
              << "      --vid <hex>              HID vendor id, default 0x0581\n"
              << "      --pid <hex>              HID product id, default 0x2581\n"
              << "      --usage-page <hex|dec>   HID usage page filter, default 0x81\n"
              << "      --usage <hex|dec>        HID usage filter\n"
              << "      --path, --hid-path <path> HID path from list-hid\n"
              << "      --serial <value>         HID serial value for VID/PID open\n"
              << "      --endpoint <value>       Transport endpoint value\n"
              << "      --wire <mode>            Wire mode: framed-binary, websocket-json-rpc\n"
              << "      --encoding <format>      RPC encoding: json, tlv, raw\n"
              << "      --registry-file <file>   Load an additional method registry JSON file\n"
              << "      --timeout <ms>           RPC timeout in milliseconds\n"
              << "      --report-id <id>         HID report id, default 0x05\n"
              << "      --input-report-size <n>  HIDAPI input buffer bytes incl report id\n"
              << "      --read-buffer-size <n>   HIDAPI read buffer bytes\n"
              << "      --output-report-size <n> HIDAPI output buffer bytes incl report id\n"
              << "      --max-reports-per-poll <n> HID read limit per poll\n"
              << "      --no-app-ready           Skip app-ready before a HID call\n"
              << "      --random-seed <hex|dec>  Override Identify randomSeed for app-ready\n"
              << "      --sid <value>            JSON envelope sid when --no-app-ready is used\n"
              << "      --log                    Write a local axtpctl log beside the executable\n"
              << "      --no-log                 Disable local file logging\n"
              << "      --log-body               Include request/response bodies in local log\n"
              << "      --verbose                Enable verbose diagnostics\n"
              << "  -h, --help                   Show this help\n"
              << "\n"
              << "Commands:\n"
              << "  call [method] [--method-id ID] [--json JSON|--json-file FILE|--tlv-hex "
                 "HEX|--raw-hex HEX]\n"
              << "  capability methods\n"
              << "  list-methods\n"
              << "  handshake\n"
              << "  firmware update --file PATH [--file-id ID] [--target TARGET]\n"
              << "  list-hid [--vid VID --pid PID --usage-page PAGE --usage USAGE]\n"
              << "  read-hid [--path PATH] [--timeout MS]\n"
              << "  ping\n"
              << "  inspect frame --hex HEX\n"
              << "\n"
              << "Examples:\n"
              << "  axtpctl -c audio.getAlgorithmConfig\n"
              << "  axtpctl -c audio.getAlgorithmCapabilities\n"
              << "  axtpctl -c audio.setAlgorithmConfig --json "
                 "'{\"noiseSuppression\":{\"enabled\":true,\"level\":3}}'\n"
              << "  axtpctl handshake -t hid\n"
              << "  axtpctl -t hid list-hid\n"
              << "  axtpctl -t hid read-hid --timeout 10000\n"
              << "  axtpctl -t hid read-hid --path \"<path from list-hid>\" --timeout 10000\n"
              << "  axtpctl -t hid -c audio.getAlgorithmConfig\n"
              << "  axtpctl -t tcp --host 127.0.0.1 --port 9000 -c audio.getAlgorithmConfig -o json\n"
              << "  axtpctl -t hid firmware update --file firmware.bin --file-id firmware\n";
}

bool isOption(const std::string& text) {
    return !text.empty() && text[0] == '-';
}

std::optional<std::string> optionValue(const std::vector<std::string>& args,
                                       const std::string& name) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return args[i + 1];
        }
    }
    return std::nullopt;
}

std::optional<std::string> firstOptionValue(const std::vector<std::string>& args,
                                            const std::vector<std::string>& names) {
    for (const auto& name : names) {
        if (const auto value = optionValue(args, name)) {
            return value;
        }
    }
    return std::nullopt;
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

bool writeBinaryFile(const std::string& path, const axtp::Bytes& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return true;
}

bool parseGlobalOptions(int argc, char** argv, CliOptions* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "--help" || arg == "-h") {
            options->command = {"help"};
            return true;
        }
        if (arg == "--verbose") {
            options->verbose = true;
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
            if (!value.has_value()) {
                return false;
            }
            options->shortcutMethod = *value;
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
        if (arg == "-t" || arg == "--transport") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            options->transport = *value;
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
        if (arg == "--endpoint" || arg == "--wire" || arg == "--registry-file" ||
            arg == "--encoding" || arg == "--timeout" || arg == "--host" || arg == "--port" ||
            arg == "--vid" || arg == "--pid" || arg == "--path" || arg == "--hid-path" ||
            arg == "--serial" || arg == "--usage-page" || arg == "--usagepage" ||
            arg == "--hid-usage-page" || arg == "--hid-usagepage" ||
            arg == "--usage" || arg == "--hid-usage" || arg == "--sid" ||
            arg == "--random-seed" || arg == "--report-id" || arg == "--input-report-size" ||
            arg == "--read-buffer-size" || arg == "--output-report-size" ||
            arg == "--max-reports-per-poll") {
            const auto value = requireValue(arg.c_str());
            if (!value.has_value()) {
                return false;
            }
            if (arg == "--endpoint") {
                options->endpoint = *value;
            } else if (arg == "--wire") {
                options->wire = *value;
            } else if (arg == "--encoding") {
                options->encoding = *value;
            } else if (arg == "--registry-file") {
                options->registryFile = *value;
            } else if (arg == "--timeout") {
                const auto parsed = parseUint32(*value);
                if (!parsed.has_value()) {
                    std::cerr << "invalid --timeout\n";
                    return false;
                }
                options->timeoutMs = *parsed;
            } else if (arg == "--host") {
                options->host = *value;
            } else if (arg == "--port") {
                options->port = parseUint32(*value);
                if (!options->port.has_value() || *options->port > 65535) {
                    std::cerr << "invalid --port\n";
                    return false;
                }
            } else if (arg == "--vid") {
                options->vid = parseUint32(*value);
                if (!options->vid.has_value() || *options->vid > 0xFFFF) {
                    std::cerr << "invalid --vid\n";
                    return false;
                }
            } else if (arg == "--pid") {
                options->pid = parseUint32(*value);
                if (!options->pid.has_value() || *options->pid > 0xFFFF) {
                    std::cerr << "invalid --pid\n";
                    return false;
                }
            } else if (arg == "--usage-page" || arg == "--usagepage" ||
                       arg == "--hid-usage-page" || arg == "--hid-usagepage") {
                options->usagePage = parseUint32(*value);
                if (!options->usagePage.has_value() || *options->usagePage > 0xFFFF) {
                    std::cerr << "invalid --usage-page\n";
                    return false;
                }
            } else if (arg == "--usage" || arg == "--hid-usage") {
                options->usage = parseUint32(*value);
                if (!options->usage.has_value() || *options->usage > 0xFFFF) {
                    std::cerr << "invalid --usage\n";
                    return false;
                }
            } else if (arg == "--path" || arg == "--hid-path") {
                options->path = *value;
            } else if (arg == "--serial") {
                options->serialNumber = *value;
            } else if (arg == "--sid") {
                options->sid = *value;
            } else if (arg == "--random-seed") {
                options->randomSeed = parseUint32(*value);
                if (!options->randomSeed.has_value()) {
                    std::cerr << "invalid --random-seed\n";
                    return false;
                }
            } else if (arg == "--report-id") {
                const auto parsed = parseUint32(*value);
                if (!parsed.has_value() || *parsed > 0xFF) {
                    std::cerr << "invalid --report-id\n";
                    return false;
                }
                options->reportId = *parsed;
            } else if (arg == "--input-report-size") {
                const auto parsed = parseUint32(*value);
                if (!parsed.has_value() || *parsed == 0) {
                    std::cerr << "invalid --input-report-size\n";
                    return false;
                }
                options->inputReportSize = *parsed;
            } else if (arg == "--read-buffer-size") {
                const auto parsed = parseUint32(*value);
                if (!parsed.has_value() || *parsed == 0) {
                    std::cerr << "invalid --read-buffer-size\n";
                    return false;
                }
                options->readBufferSize = *parsed;
            } else if (arg == "--output-report-size") {
                const auto parsed = parseUint32(*value);
                if (!parsed.has_value() || *parsed == 0) {
                    std::cerr << "invalid --output-report-size\n";
                    return false;
                }
                options->outputReportSize = *parsed;
            } else if (arg == "--max-reports-per-poll") {
                const auto parsed = parseUint32(*value);
                if (!parsed.has_value() || *parsed == 0) {
                    std::cerr << "invalid --max-reports-per-poll\n";
                    return false;
                }
                options->maxReportsPerPoll = *parsed;
            }
            continue;
        }
        options->command.push_back(arg);
    }

    if (options->shortcutMethod.has_value()) {
        if (options->shortcutMethod->empty()) {
            std::cerr << "method name must not be empty\n";
            return false;
        }
        if (!options->command.empty()) {
            std::cerr << "-c/--command cannot be combined with an explicit command\n";
            return false;
        }
        options->command = {"call", *options->shortcutMethod};
    }
    return true;
}

std::uint16_t readU16Be(const axtp::Bytes& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] << 8) |
           static_cast<std::uint16_t>(bytes[offset + 1]);
}

std::string payloadTypeName(std::uint8_t type) {
    if (type == static_cast<std::uint8_t>(axtp::PayloadType::Control)) {
        return "control";
    }
    if (type == static_cast<std::uint8_t>(axtp::PayloadType::Rpc)) {
        return "rpc";
    }
    if (type == static_cast<std::uint8_t>(axtp::PayloadType::Stream)) {
        return "stream";
    }
    return "unknown";
}

int inspectFrame(const std::vector<std::string>& args) {
    const auto hex = optionValue(args, "--hex");
    if (!hex.has_value()) {
        std::cerr << "inspect frame requires --hex\n";
        return 2;
    }
    const auto bytes = parseHex(*hex);
    if (!bytes.has_value()) {
        std::cerr << "invalid hex\n";
        return 2;
    }
    if (bytes->size() < axtp::kStandardFrameHeaderSize) {
        std::cerr << "frame too short\n";
        return 2;
    }

    auto object = nlohmann::json::object();
    object["magic"] =
        ((*bytes)[0] == axtp::kAxtpStandardMagic0 && (*bytes)[1] == axtp::kAxtpStandardMagic1)
            ? "AX"
            : "invalid";
    object["version"] = (*bytes)[2];
    object["payloadType"] = payloadTypeName((*bytes)[3]);
    const auto payloadLength = readU16Be(*bytes, 4);
    object["payloadLength"] = payloadLength;
    object["sourceId"] = (*bytes)[6];
    object["destinationId"] = (*bytes)[7];
    object["messageId"] = readU16Be(*bytes, 8);
    object["frameIndex"] = (*bytes)[10];
    object["frameCount"] = (*bytes)[11];

    const auto total = static_cast<std::size_t>(axtp::kStandardFrameHeaderSize) + payloadLength +
                       axtp::kStandardFrameCrcSize;
    object["complete"] = bytes->size() >= total;
    if (bytes->size() >= total) {
        const auto expected = readU16Be(*bytes, total - axtp::kStandardFrameCrcSize);
        const auto actual =
            axtp::crc16CcittFalse(bytes->data(), total - axtp::kStandardFrameCrcSize);
        object["crcExpected"] = expected;
        object["crcActual"] = actual;
        object["crcOk"] = expected == actual;
    }

    std::cout << object.dump() << "\n";
    return 0;
}

nlohmann::json parseJsonValueOrString(const axtp::Bytes& bytes) {
    if (bytes.empty()) {
        return nullptr;
    }
    try {
        const std::string text(bytes.begin(), bytes.end());
        return nlohmann::json::parse(text);
    } catch (const std::exception&) {
        const std::string text(bytes.begin(), bytes.end());
        return text;
    }
}

bool validateJson(std::string_view text) {
    try {
        const auto parsed = nlohmann::json::parse(text);
        (void)parsed;
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "invalid JSON params: " << ex.what() << "\n";
        return false;
    }
}

OutputFormat parseOutputFormat(const std::string& value) {
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

void printJsonObject(const nlohmann::json& object, OutputFormat format) {
    if (format == OutputFormat::Pretty) {
        std::cout << object.dump() << "\n";
        return;
    }
    std::cout << object.dump() << "\n";
}

std::string md5Hex(const axtp::Bytes& bytes) {
    websocketpp::md5::md5_state_t state;
    websocketpp::md5::md5_init(&state);
    if (!bytes.empty()) {
        websocketpp::md5::md5_append(
            &state,
            reinterpret_cast<const websocketpp::md5::md5_byte_t*>(bytes.data()),
            bytes.size());
    }
    websocketpp::md5::md5_byte_t digest[16] = {};
    websocketpp::md5::md5_finish(&state, digest);
    return toHex(axtp::Bytes(digest, digest + 16));
}

std::optional<std::string> jsonFieldString(const nlohmann::json& object, const char* field) {
    if (!object.is_object() || !object.contains(field) || !object[field].is_string()) {
        return std::nullopt;
    }
    return object[field].get<std::string>();
}

std::string errorName(axtp::ErrorCode code) {
    return axtp::toolkit::errorName(code);
}

void installMockHandlers(axtp::sdk::AxtpClient& client) {
    client.registerMethod(
        static_cast<std::uint16_t>(axtp::MethodId::AudioGetAlgorithmConfig),
        [](const axtp::RpcPayload&) {
            const std::string body =
                R"({"noiseSuppression":{"enabled":true,"level":3},"echoCancellation":{"enabled":true}})";
            return axtp::Bytes(body.begin(), body.end());
        });
    client.registerMethod(static_cast<std::uint16_t>(axtp::MethodId::AudioSetAlgorithmConfig),
                          [](const axtp::RpcPayload&) { return axtp::Bytes{}; });
    client.registerMethod(static_cast<std::uint16_t>(axtp::MethodId::AudioGetAlgorithmCapabilities),
                          [](const axtp::RpcPayload&) {
                              const std::string body =
                                  R"({"algorithms":{"noiseSuppression":{"level":{"min":0,"max":5}}}})";
                              return axtp::Bytes(body.begin(), body.end());
                          });
    client.registerMethod(static_cast<std::uint16_t>(axtp::MethodId::AudioResetAlgorithmConfig),
                          [](const axtp::RpcPayload&) { return axtp::Bytes{}; });
    client.registerMethod(
        static_cast<std::uint16_t>(axtp::MethodId::FirmwareBeginUpdate),
        [](const axtp::RpcPayload& request) {
            std::string fileId = "firmware";
            const auto params = parseJsonValueOrString(request.body);
            if (params.is_object() && params.contains("manifest") &&
                params["manifest"].contains("files") && params["manifest"]["files"].is_array() &&
                !params["manifest"]["files"].empty()) {
                if (const auto parsed =
                        jsonFieldString(params["manifest"]["files"].front(), "fileId")) {
                    fileId = *parsed;
                }
            }

            auto response = nlohmann::json::object();
            response["updateSessionId"] = "mock-update-1";
            response["state"] = "receiving";
            response["streams"] =
                nlohmann::json::array({{{"fileId", fileId}, {"streamId", 0x1001}}});
            response["chunkSize"] = 4;
            const auto body = response.dump();
            return axtp::Bytes(body.begin(), body.end());
        });
    client.registerMethod(
        static_cast<std::uint16_t>(axtp::MethodId::FirmwareFinishUpdate),
        [](const axtp::RpcPayload& request) {
            std::string updateSessionId = "mock-update-1";
            const auto params = parseJsonValueOrString(request.body);
            if (const auto parsed = jsonFieldString(params, "updateSessionId")) {
                updateSessionId = *parsed;
            }

            auto response = nlohmann::json::object();
            response["updateSessionId"] = updateSessionId;
            response["accepted"] = true;
            response["state"] = "verifying";
            const auto body = response.dump();
            return axtp::Bytes(body.begin(), body.end());
        });
}

axtp::toolkit::OutputFormat toolkitFormat(OutputFormat format) {
    switch (format) {
    case OutputFormat::Json:
        return axtp::toolkit::OutputFormat::Json;
    case OutputFormat::Hex:
        return axtp::toolkit::OutputFormat::Hex;
    case OutputFormat::File:
        return axtp::toolkit::OutputFormat::File;
    case OutputFormat::Pretty:
    default:
        return axtp::toolkit::OutputFormat::Pretty;
    }
}

axtp::toolkit::HidOpenOptions hidOptionsFromCli(
    const CliOptions& options,
    std::function<void(const axtp::HidReportTrace&)> trace = {}) {
    axtp::toolkit::HidOpenOptions hid;
    hid.vendorId = options.vid;
    hid.productId = options.pid;
    hid.usagePage = options.usagePage;
    hid.usage = options.usage;
    hid.devicePath = options.path;
    hid.serialNumber = options.serialNumber;
    hid.reportId = options.reportId;
    hid.inputReportSize = options.inputReportSize;
    hid.readBufferSize = options.readBufferSize;
    hid.outputReportSize = options.outputReportSize;
    hid.maxReportsPerPoll = options.maxReportsPerPoll;
    hid.useReadThread = true;
    hid.readThreadTimeoutMs = 1000;
    hid.reportTrace = std::move(trace);
    return hid;
}

axtp::toolkit::TransportOpenOptions transportOptionsFromCli(
    const CliOptions& options,
    std::function<void(const axtp::HidReportTrace&)> trace = {}) {
    axtp::toolkit::TransportOpenOptions transport;
    transport.kind = options.transport;
    transport.host = options.host;
    transport.port = options.port;
    transport.hid = hidOptionsFromCli(options, std::move(trace));
    return transport;
}

bool isHidTransport(const CliOptions& options) {
    return options.transport == "hid" || options.transport == "hidapi";
}

bool attachTransport(const CliOptions& options,
                     axtp::sdk::AxtpClient* client,
                     std::function<void(const axtp::HidReportTrace&)> trace = {}) {
    auto bundle = axtp::toolkit::makeTransport(transportOptionsFromCli(options, std::move(trace)));
    if (!bundle.transport) {
        std::cerr << "unsupported transport: " << options.transport << "\n";
        return false;
    }
    client->attachTransport(std::move(bundle.transport));
    return client->isConnected();
}

std::string hidTraceKindName(axtp::HidReportTraceKind kind) {
    switch (kind) {
    case axtp::HidReportTraceKind::ReadReport:
        return "read-report";
    case axtp::HidReportTraceKind::ReadTimeout:
        return "read-timeout";
    case axtp::HidReportTraceKind::ReadError:
        return "read-error";
    case axtp::HidReportTraceKind::WriteFrame:
        return "write-frame";
    case axtp::HidReportTraceKind::WriteReport:
        return "write-report";
    case axtp::HidReportTraceKind::WriteError:
        return "write-error";
    case axtp::HidReportTraceKind::AcceptedReport:
        return "accepted-report";
    case axtp::HidReportTraceKind::DroppedReportId:
        return "dropped-report-id";
    }
    return "unknown";
}

std::string hidTraceLine(const axtp::HidReportTrace& trace, bool includeBody) {
    std::ostringstream out;
    out << "hid " << hidTraceKindName(trace.kind)
        << " size=" << trace.size
        << " reportId=" << axtp::toolkit::toHexByte(trace.reportId);
    if (trace.expectedReportId != 0) {
        out << " expectedReportId=" << axtp::toolkit::toHexByte(trace.expectedReportId);
    }
    if (trace.timeoutMs != 0) {
        out << " timeoutMs=" << trace.timeoutMs;
    }
    if (!trace.message.empty()) {
        out << " message=" << trace.message;
    }
    if (includeBody && trace.data != nullptr && trace.size > 0) {
        axtp::Bytes body(trace.data, trace.data + trace.size);
        out << " data=" << toHex(body);
    }
    return out.str();
}

void printHidTrace(const axtp::HidReportTrace& trace, OutputFormat format, bool includeBody) {
    const bool hasBytes = trace.data != nullptr && trace.size > 0;
    if (format == OutputFormat::Json) {
        auto object = nlohmann::json::object();
        object["kind"] = hidTraceKindName(trace.kind);
        object["size"] = trace.size;
        object["reportId"] = trace.reportId;
        object["expectedReportId"] = trace.expectedReportId;
        object["timeoutMs"] = trace.timeoutMs;
        object["message"] = trace.message;
        if (hasBytes && includeBody) {
            axtp::Bytes body(trace.data, trace.data + trace.size);
            object["hex"] = toHex(body);
        }
        std::cout << object.dump() << "\n";
        return;
    }
    if (trace.kind == axtp::HidReportTraceKind::ReadReport ||
        trace.kind == axtp::HidReportTraceKind::AcceptedReport ||
        trace.kind == axtp::HidReportTraceKind::DroppedReportId ||
        trace.kind == axtp::HidReportTraceKind::ReadError ||
        trace.kind == axtp::HidReportTraceKind::WriteError) {
        std::cout << hidTraceLine(trace, includeBody) << "\n";
    }
}

axtp::RpcEncoding encodingFromName(const std::string& name) {
    if (name == "tlv") {
        return axtp::jsonBinaryRpcEncoding();
    }
    if (name == "raw") {
        return axtp::jsonBinaryRpcEncoding();
    }
    return axtp::RpcEncoding::Json;
}

bool buildCallBody(const CliOptions& options,
                   const std::vector<std::string>& args,
                   axtp::RpcEncoding* encoding,
                   axtp::Bytes* body) {
    const auto commandJson = firstOptionValue(args, {"--json", "-j", "--params"});
    const auto commandJsonFile = firstOptionValue(args, {"--json-file", "-f", "--params-file"});
    const bool hasJson = options.json.has_value() || commandJson.has_value();
    const bool hasJsonFile = options.jsonFile.has_value() || commandJsonFile.has_value();
    if (hasJson && hasJsonFile) {
        std::cerr << "--json and --json-file cannot be used together\n";
        return false;
    }

    *encoding = encodingFromName(options.encoding);
    body->assign({'{', '}'});

    if (hasJson) {
        const auto text = options.json.value_or(*commandJson);
        if (!validateJson(text)) {
            return false;
        }
        body->assign(text.begin(), text.end());
        *encoding = axtp::RpcEncoding::Json;
        return true;
    }
    if (hasJsonFile) {
        const auto path = options.jsonFile.value_or(*commandJsonFile);
        const auto contents = readTextFile(path);
        if (!contents.has_value()) {
            std::cerr << "failed to read JSON params file: " << path << "\n";
            return false;
        }
        if (!validateJson(*contents)) {
            return false;
        }
        body->assign(contents->begin(), contents->end());
        *encoding = axtp::RpcEncoding::Json;
        return true;
    }
    if (const auto hex = optionValue(args, "--tlv-hex")) {
        auto parsed = parseHex(*hex);
        if (!parsed.has_value()) {
            std::cerr << "invalid --tlv-hex\n";
            return false;
        }
        *body = std::move(*parsed);
        *encoding = axtp::jsonBinaryRpcEncoding();
        return true;
    }
    if (const auto file = optionValue(args, "--tlv-file")) {
        const auto contents = readBinaryFile(*file);
        if (!contents.has_value()) {
            std::cerr << "failed to read tlv file: " << *file << "\n";
            return false;
        }
        *body = *contents;
        *encoding = axtp::jsonBinaryRpcEncoding();
        return true;
    }
    if (const auto hex = optionValue(args, "--raw-hex")) {
        auto parsed = parseHex(*hex);
        if (!parsed.has_value()) {
            std::cerr << "invalid --raw-hex\n";
            return false;
        }
        *body = std::move(*parsed);
        *encoding = axtp::jsonBinaryRpcEncoding();
        return true;
    }
    if (const auto file = optionValue(args, "--raw-file")) {
        const auto contents = readBinaryFile(*file);
        if (!contents.has_value()) {
            std::cerr << "failed to read raw file: " << *file << "\n";
            return false;
        }
        *body = *contents;
        *encoding = axtp::jsonBinaryRpcEncoding();
        return true;
    }
    return true;
}

void printAppReadyTrace(const axtp::sdk::AppReadyTraceEvent& event,
                        bool includeBody,
                        bool verbose) {
    if (!verbose) {
        return;
    }
    std::cerr << "APP_READY " << event.stage << ":" << event.action
              << " status=" << errorName(event.statusCode);
    if (event.controlId != 0) {
        std::cerr << " controlId=" << event.controlId;
    }
    if (!event.sid.empty()) {
        std::cerr << " sid=" << event.sid;
    }
    if (event.hasRandomSeed) {
        std::cerr << " randomSeed=" << axtp::toolkit::toHexU32(event.randomSeed);
    }
    if (!event.detail.empty()) {
        std::cerr << " detail=" << event.detail;
    }
    if (includeBody && !event.bodyText.empty()) {
        std::cerr << " body=" << event.bodyText;
    }
    std::cerr << "\n";
}

std::string appReadyTraceLine(const axtp::sdk::AppReadyTraceEvent& event, bool includeBody) {
    std::ostringstream out;
    out << "app-ready stage=" << event.stage
        << " action=" << event.action
        << " status=" << errorName(event.statusCode);
    if (event.controlId != 0) {
        out << " controlId=" << event.controlId;
    }
    if (!event.sid.empty()) {
        out << " sid=" << event.sid;
    }
    if (event.hasRandomSeed) {
        out << " randomSeed=" << axtp::toolkit::toHexU32(event.randomSeed);
    }
    if (!event.detail.empty()) {
        out << " detail=" << event.detail;
    }
    if (includeBody && !event.bodyText.empty()) {
        out << " body=" << event.bodyText;
    }
    return out.str();
}

int printAppReadyResult(const axtp::sdk::AppReadyResult& result,
                        std::chrono::milliseconds elapsed,
                        OutputFormat format) {
    if (format == OutputFormat::Json) {
        auto output = nlohmann::json::object();
        output["ok"] = result.ok;
        output["stage"] = result.stage;
        output["statusCode"] = static_cast<std::uint16_t>(result.statusCode);
        output["status"] = errorName(result.statusCode);
        output["sid"] = result.sid;
        if (result.hasRandomSeed) {
            output["randomSeed"] = result.randomSeed;
            output["randomSeedHex"] = axtp::toolkit::toHexU32(result.randomSeed);
        }
        output["elapsedMs"] = elapsed.count();
        std::cout << output.dump() << "\n";
        return result.ok ? 0 : 4;
    }
    if (!result.ok) {
        std::cerr << "AXTP app-ready failed at " << result.stage << ": "
                  << errorName(result.statusCode) << " ("
                  << static_cast<std::uint16_t>(result.statusCode) << ")\n";
        return 4;
    }
    std::cout << "APP_READY sid=" << result.sid;
    if (result.hasRandomSeed) {
        std::cout << " randomSeed=" << axtp::toolkit::toHexU32(result.randomSeed);
    }
    std::cout << " elapsedMs=" << elapsed.count() << "\n";
    return 0;
}

axtp::toolkit::Logger makeLogger(const CliOptions& options) {
    return axtp::toolkit::Logger(options.executablePath, "axtpctl", options.logEnabled, options.logBody);
}

int callMethod(const CliOptions& options) {
    std::optional<std::string> methodName;
    if (options.command.size() >= 2 && !isOption(options.command[1])) {
        methodName = options.command[1];
    }
    if (methodName.has_value() && methodName->empty()) {
        std::cerr << "method name must not be empty\n";
        return 2;
    }

    std::optional<std::uint32_t> methodId;
    if (const auto rawId = optionValue(options.command, "--method-id")) {
        methodId = parseUint32(*rawId);
        if (!methodId.has_value()) {
            std::cerr << "invalid --method-id\n";
            return 2;
        }
    }

    axtp::RpcEncoding encoding = axtp::RpcEncoding::Json;
    axtp::Bytes body;
    if (!buildCallBody(options, options.command, &encoding, &body)) {
        return 2;
    }

    auto logger = makeLogger(options);
    std::mutex traceMutex;
    auto hidTrace = [&logger, &options, &traceMutex](const axtp::HidReportTrace& trace) {
        logger.write(hidTraceLine(trace, logger.includeBody()));
        if (options.verbose) {
            std::lock_guard<std::mutex> lock(traceMutex);
            printHidTrace(trace, parseOutputFormat(options.output), logger.includeBody());
        }
    };

    axtp::sdk::ClientOptions clientOptions;
    clientOptions.autoIdentify = !options.noAppReady;
    axtp::sdk::AxtpClient client(clientOptions);
    if (!options.registryFile.empty()) {
        auto registry = axtp::MethodRegistryJson::fromFile(options.registryFile);
        for (const auto& entry : registry.entries()) {
            client.registry().addMethod(entry.id, entry.name);
        }
    }

    if (!methodId.has_value() && methodName.has_value()) {
        methodId = client.registry().findMethodId(*methodName);
    }
    if (!methodName.has_value() && !methodId.has_value()) {
        std::cerr << "call requires a method name or --method-id\n";
        return 2;
    }
    if (!methodId.has_value()) {
        std::cerr << "Unknown method: " << *methodName
                  << "\nRun `axtpctl list-methods` to view available methods.\n";
        return 3;
    }

    if (!attachTransport(options, &client, hidTrace)) {
        std::cerr << "failed to connect transport: " << options.transport << "\n";
        return 4;
    }
    if (options.transport == "mock") {
        client.registerMethod(*methodId,
                              [](const axtp::RpcPayload& request) { return request.body; });
        installMockHandlers(client);
    }

    if (isHidTransport(options) && !options.noAppReady) {
        axtp::sdk::AppReadyOptions appOptions;
        appOptions.timeout = std::chrono::milliseconds(options.timeoutMs);
        appOptions.randomSeed = options.randomSeed;
        appOptions.trace = [&logger, &options](const axtp::sdk::AppReadyTraceEvent& event) {
            logger.write(appReadyTraceLine(event, logger.includeBody()));
            printAppReadyTrace(event, logger.includeBody(), options.verbose);
        };
        const auto ready = client.ensureAppReady(appOptions);
        if (!ready.ok) {
            const auto outputFormat = parseOutputFormat(options.output);
            return printAppReadyResult(ready, std::chrono::milliseconds(0), outputFormat);
        }
    }

    axtp::RpcPayload request;
    request.encoding = encoding;
    request.op = axtp::RpcOp::Request;
    request.methodOrEventId = *methodId;
    request.bodyEncoding = axtp::bodyEncodingForRpcEncoding(encoding);
    request.meta.sourceProtocol =
        encoding == axtp::RpcEncoding::Json ? axtp::SourceProtocol::JsonRpc
                                            : axtp::SourceProtocol::AxtpV1;
    if (methodName.has_value()) {
        request.meta.jsonMethodOrEventName = *methodName;
    }
    if (options.noAppReady && encoding == axtp::RpcEncoding::Json) {
        request.meta.jsonSid = options.sid;
    }
    request.body = std::move(body);

    axtp::sdk::CallOptions callOptions;
    callOptions.timeout = std::chrono::milliseconds(options.timeoutMs);
    callOptions.encoding = request.encoding;
    auto response = client.callRaw(std::move(request), callOptions);

    const auto outputMode =
        firstOptionValue(options.command, {"--output", "-o"}).value_or(options.output);
    const auto outputFormat = parseOutputFormat(outputMode);
    if (outputFormat == OutputFormat::Hex) {
        std::cout << toHex(response.body) << "\n";
        return response.statusCode == axtp::ErrorCode::Success ? 0 : 4;
    }
    if (outputFormat == OutputFormat::File) {
        const auto path = optionValue(options.command, "--output-file");
        if (!path.has_value() || !writeBinaryFile(*path, response.body)) {
            std::cerr << "failed to write output file\n";
            return 2;
        }
        return response.statusCode == axtp::ErrorCode::Success ? 0 : 4;
    }

    auto output = nlohmann::json::object();
    output["ok"] = response.statusCode == axtp::ErrorCode::Success;
    if (methodName.has_value()) {
        output["method"] = *methodName;
    }
    output["methodId"] = *methodId;
    output["requestId"] = response.requestId;
    if (response.statusCode == axtp::ErrorCode::Success) {
        if (!response.body.empty()) {
            if (response.encoding == axtp::RpcEncoding::Json) {
                output["result"] = parseJsonValueOrString(response.body);
            } else {
                output["resultHex"] = toHex(response.body);
            }
        }
    } else {
        auto error = nlohmann::json::object();
        error["code"] = errorName(response.statusCode);
        error["numericCode"] = static_cast<std::uint16_t>(response.statusCode);
        error["message"] = errorName(response.statusCode);
        output["error"] = std::move(error);
    }
    printJsonObject(output, outputFormat);
    return response.statusCode == axtp::ErrorCode::Success ? 0 : 4;
}

int firmwareUpdateCommand(const CliOptions& options) {
    if (options.command.size() < 2 || options.command[1] != "update") {
        std::cerr << "firmware requires subcommand: update\n";
        return 2;
    }
    if (options.transport == "websocket" || options.transport == "ws") {
        std::cerr << "firmware update requires a framed-binary transport\n";
        return 2;
    }

    const auto filePath = optionValue(options.command, "--file");
    if (!filePath.has_value() || filePath->empty()) {
        std::cerr << "firmware update requires --file\n";
        return 2;
    }

    const auto image = readBinaryFile(*filePath);
    if (!image.has_value()) {
        std::cerr << "failed to read firmware file: " << *filePath << "\n";
        return 2;
    }

    std::uint32_t chunkSize = 1024;
    if (const auto rawChunkSize = optionValue(options.command, "--chunk-size")) {
        const auto parsed = parseUint32(*rawChunkSize);
        if (!parsed.has_value() || *parsed == 0) {
            std::cerr << "invalid --chunk-size\n";
            return 2;
        }
        chunkSize = *parsed;
    }

    const auto outputFormat = parseOutputFormat(options.output);
    const auto fileId = optionValue(options.command, "--file-id").value_or("firmware");
    const auto target = optionValue(options.command, "--target");
    const auto packageId = optionValue(options.command, "--package-id");
    const auto version = optionValue(options.command, "--version");
    const auto md5 = md5Hex(*image);

    auto logger = makeLogger(options);
    std::mutex traceMutex;
    auto hidTrace = [&logger, &options, &traceMutex](const axtp::HidReportTrace& trace) {
        logger.write(hidTraceLine(trace, logger.includeBody()));
        if (options.verbose) {
            std::lock_guard<std::mutex> lock(traceMutex);
            printHidTrace(trace, parseOutputFormat(options.output), logger.includeBody());
        }
    };

    axtp::sdk::ClientOptions clientOptions;
    clientOptions.autoIdentify = !options.noAppReady;
    axtp::sdk::AxtpClient client(clientOptions);
    if (!attachTransport(options, &client, hidTrace)) {
        std::cerr << "failed to connect transport: " << options.transport << "\n";
        return 4;
    }
    if (options.transport == "mock") {
        installMockHandlers(client);
    }

    if (isHidTransport(options) && !options.noAppReady) {
        axtp::sdk::AppReadyOptions appOptions;
        appOptions.timeout = std::chrono::milliseconds(options.timeoutMs);
        appOptions.randomSeed = options.randomSeed;
        appOptions.trace = [&logger, &options](const axtp::sdk::AppReadyTraceEvent& event) {
            logger.write(appReadyTraceLine(event, logger.includeBody()));
            printAppReadyTrace(event, logger.includeBody(), options.verbose);
        };
        const auto ready = client.ensureAppReady(appOptions);
        if (!ready.ok) {
            return printAppReadyResult(ready, std::chrono::milliseconds(0), outputFormat);
        }
    }

    axtp::sdk::CallOptions callOptions;
    callOptions.timeout = std::chrono::milliseconds(options.timeoutMs);
    callOptions.encoding = axtp::RpcEncoding::Json;

    axtp::firmware::FirmwareUpdateRequest updateRequest;
    updateRequest.file.fileId = fileId;
    if (target.has_value()) {
        updateRequest.file.target = *target;
    }
    updateRequest.file.data = *image;
    updateRequest.file.md5 = md5;
    if (packageId.has_value()) {
        updateRequest.packageId = *packageId;
    }
    if (version.has_value()) {
        updateRequest.version = *version;
    }
    updateRequest.preferredChunkSize = chunkSize;
    if (options.noAppReady) {
        updateRequest.jsonSid = options.sid;
    }

    axtp::firmware::FirmwareUpdateProfile profile(client);
    const auto result = profile.update(updateRequest, callOptions);

    auto output = nlohmann::json::object();
    output["ok"] = result.ok;
    output["method"] = "firmware.update";
    output["file"] = *filePath;
    output["fileId"] = fileId;
    output["size"] = image->size();
    output["md5"] = md5;
    output["updateSessionId"] = result.updateSessionId;
    output["streamId"] = result.streamId;
    output["chunkSize"] = result.chunkSize;
    output["chunks"] = result.chunks;
    output["begin"] = result.begin;
    output["finish"] = result.finish;
    if (!result.ok) {
        auto error = nlohmann::json::object();
        error["code"] = errorName(result.status);
        error["numericCode"] = static_cast<std::uint16_t>(result.status);
        error["message"] = errorName(result.status);
        error["method"] = result.failedMethod;
        output["error"] = std::move(error);
    }
    printJsonObject(output, outputFormat);
    return result.ok ? 0 : 4;
}

int listHidDevicesCommand(const CliOptions& options) {
    if (!isHidTransport(options)) {
        std::cerr << "list-hid requires -t hid\n";
        return 2;
    }
    axtp::toolkit::printHidDevices(
        hidOptionsFromCli(options), toolkitFormat(parseOutputFormat(options.output)), std::cout);
    return 0;
}

class CountingByteSink final : public axtp::IByteSink {
public:
    void onBytes(const axtp::Byte* data, std::size_t size) override {
        (void)data;
        chunks.fetch_add(1);
        bytes.fetch_add(static_cast<std::uint64_t>(size));
    }

    std::atomic<std::uint64_t> chunks{0};
    std::atomic<std::uint64_t> bytes{0};
};

int readHidCommand(const CliOptions& options) {
    if (!isHidTransport(options)) {
        std::cerr << "read-hid requires -t hid\n";
        return 2;
    }

    auto hidOpen = hidOptionsFromCli(options);
    if (!axtp::toolkit::hasHidTarget(hidOpen)) {
        std::cerr << "read-hid requires --path/--hid-path or both --vid and --pid\n";
        return 2;
    }

    auto logger = makeLogger(options);
    std::mutex traceMutex;
    const auto format = parseOutputFormat(options.output);
    hidOpen.reportTrace = [&logger, &traceMutex, format](const axtp::HidReportTrace& trace) {
        logger.write(hidTraceLine(trace, logger.includeBody()));
        std::lock_guard<std::mutex> lock(traceMutex);
        printHidTrace(trace, format, true);
    };

    auto transport = std::make_unique<axtp::HidTransport>(
        axtp::toolkit::makeHidTransportOptions(hidOpen));
    CountingByteSink sink;
    transport->bind(sink);
    transport->open();
    if (!transport->isOpen()) {
        std::cerr << "failed to open HID device";
        if (!options.path.empty()) {
            std::cerr << " path=" << options.path;
        }
        if (options.vid.has_value() || options.pid.has_value()) {
            std::cerr << " vid=" << axtp::toolkit::toHexId(options.vid.value_or(0))
                      << " pid=" << axtp::toolkit::toHexId(options.pid.value_or(0));
        }
        if (options.usagePage.has_value()) {
            std::cerr << " usagePage=" << axtp::toolkit::toHexId(*options.usagePage);
        }
        if (options.usage.has_value()) {
            std::cerr << " usage=" << axtp::toolkit::toHexId(*options.usage);
        }
        std::cerr << "\n";
        return 4;
    }

    std::cerr << "read-hid opened; sending is disabled; ";
    if (options.timeoutMs == 0) {
        std::cerr << "reading until interrupted\n";
    } else {
        std::cerr << "reading for " << options.timeoutMs << " ms\n";
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
    while (options.timeoutMs == 0 || std::chrono::steady_clock::now() < deadline) {
        transport->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto stats = transport->stats();
    transport->close();

    std::ostringstream summary;
    summary << "hid read stats readReports=" << stats.readReports
            << " readBytes=" << stats.readBytes
            << " acceptedReports=" << stats.acceptedReports
            << " droppedReportId=" << stats.droppedReportId
            << " readErrors=" << stats.readErrors
            << " queuedReports=" << stats.queuedReports
            << " sinkChunks=" << sink.chunks.load()
            << " sinkBytes=" << sink.bytes.load();
    logger.write(summary.str());
    std::cerr << summary.str() << "\n";
    return stats.readErrors == 0 ? 0 : 4;
}

int handshakeCommand(const CliOptions& options) {
    auto logger = makeLogger(options);
    std::mutex traceMutex;
    auto hidTrace = [&logger, &options, &traceMutex](const axtp::HidReportTrace& trace) {
        logger.write(hidTraceLine(trace, logger.includeBody()));
        if (options.verbose) {
            std::lock_guard<std::mutex> lock(traceMutex);
            printHidTrace(trace, parseOutputFormat(options.output), logger.includeBody());
        }
    };

    axtp::sdk::ClientOptions clientOptions;
    clientOptions.autoIdentify = false;
    axtp::sdk::AxtpClient client(clientOptions);
    if (!attachTransport(options, &client, hidTrace)) {
        std::cerr << "failed to connect transport: " << options.transport << "\n";
        return 4;
    }

    axtp::sdk::AppReadyOptions appOptions;
    appOptions.timeout = std::chrono::milliseconds(options.timeoutMs);
    appOptions.randomSeed = options.randomSeed;
    appOptions.trace = [&logger, &options](const axtp::sdk::AppReadyTraceEvent& event) {
        logger.write(appReadyTraceLine(event, logger.includeBody()));
        printAppReadyTrace(event, logger.includeBody(), options.verbose);
    };

    const auto started = std::chrono::steady_clock::now();
    const auto result = client.ensureAppReady(appOptions);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    client.close();
    return printAppReadyResult(result, elapsed, parseOutputFormat(options.output));
}

int printCapabilityMethods() {
    auto methods = nlohmann::json::array();
    for (const auto& method : axtp::kMethodRegistry) {
        auto item = nlohmann::json::object();
        item["id"] = method.id;
        item["name"] = method.name;
        item["domain"] = method.domain;
        item["requestSchema"] = method.request_schema;
        item["responseSchema"] = method.response_schema;
        methods.push_back(std::move(item));
    }
    std::cout << methods.dump() << "\n";
    return 0;
}

int ping(const CliOptions& options) {
    auto output = nlohmann::json::object();
    output["ok"] = options.transport == "mock";
    output["transport"] = options.transport;
    output["wire"] = options.wire;
    if (options.transport != "mock") {
        output["message"] = "real transport ping is not implemented in P0";
    }
    std::cout << output.dump() << "\n";
    return options.transport == "mock" ? 0 : 4;
}

int runCommand(const CliOptions& options) {
    if (options.command.empty() || options.command[0] == "help") {
        printUsage();
        return 0;
    }
    if (options.command[0] == "call") {
        return callMethod(options);
    }
    if (options.command[0] == "list-methods") {
        return printCapabilityMethods();
    }
    if (options.command[0] == "list-hid") {
        return listHidDevicesCommand(options);
    }
    if (options.command[0] == "read-hid") {
        return readHidCommand(options);
    }
    if (options.command[0] == "handshake") {
        return handshakeCommand(options);
    }
    if (options.command[0] == "firmware") {
        return firmwareUpdateCommand(options);
    }
    if (options.command[0] == "capability" && options.command.size() >= 2 &&
        options.command[1] == "methods") {
        return printCapabilityMethods();
    }
    if (options.command[0] == "ping") {
        return ping(options);
    }
    if (options.command[0] == "inspect" && options.command.size() >= 2 &&
        options.command[1] == "frame") {
        return inspectFrame(options.command);
    }
    std::cerr << "unknown command\n";
    printUsage();
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    CliOptions options;
    options.executablePath = argc > 0 && argv[0] != nullptr ? argv[0] : "axtpctl";
    try {
        if (!parseGlobalOptions(argc, argv, &options)) {
            return 2;
        }
        return runCommand(options);
    } catch (const std::exception& ex) {
        std::cerr << "axtpctl: " << ex.what() << "\n";
        return 1;
    }
}
