#include <boost/json.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "axtp.hpp"
#include "method_registry_json.hpp"
#include "testing/mock_transport.hpp"
#include "hidapi/hid_local_backend.hpp"
#include "hidapi/hid_transport.hpp"
#include "tcp_boost/tcp_transport.hpp"
#include "websocket_boost/websocket_transport.hpp"

#include "axtp_sdk_all.hpp"

namespace {

enum class OutputFormat {
    Pretty,
    Json,
    Hex,
    File,
};

struct CliOptions {
    std::string transport = "mock";
    std::string endpoint;
    std::string host = "127.0.0.1";
    std::string path;
    std::string wire = "websocket-json-rpc";
    std::string encoding = "json";
    std::string registryFile;
    std::string output = "pretty";
    std::optional<std::string> shortcutMethod;
    std::optional<std::string> json;
    std::optional<std::string> jsonFile;
    std::optional<std::uint32_t> port;
    std::optional<std::uint32_t> vid;
    std::optional<std::uint32_t> pid;
    std::uint32_t timeoutMs = 5000;
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
              << "  -t, --transport <kind>       Select transport: hid, hid-sim, tcp, websocket, mock\n"
              << "  -o, --output <format>        Output format: pretty, json\n"
              << "      --host <host>            TCP host\n"
              << "      --port <port>            TCP or WebSocket port\n"
              << "      --vid <hex>              HID vendor id\n"
              << "      --pid <hex>              HID product id\n"
              << "      --path <device-path>     HID serial/path or hid-sim socket path\n"
              << "      --endpoint <value>       Transport endpoint value\n"
              << "      --wire <mode>            Wire mode: framed-binary, websocket-json-rpc\n"
              << "      --encoding <format>      RPC encoding: json, tlv, raw\n"
              << "      --registry-file <file>   Load an additional method registry JSON file\n"
              << "      --timeout <ms>           RPC timeout in milliseconds\n"
              << "      --verbose                Enable verbose diagnostics\n"
              << "  -h, --help                   Show this help\n"
              << "\n"
              << "Commands:\n"
              << "  call [method] [--method-id ID] [--json JSON|--json-file FILE|--tlv-hex "
                 "HEX|--raw-hex HEX]\n"
              << "  capability methods\n"
              << "  list-methods\n"
              << "  ping\n"
              << "  inspect frame --hex HEX\n"
              << "\n"
              << "Examples:\n"
              << "  axtpctl -c audio.getAlgorithmConfig\n"
              << "  axtpctl -c audio.getAlgorithmCapabilities\n"
              << "  axtpctl -c audio.setAlgorithmConfig --json "
                 "'{\"noiseSuppression\":{\"enabled\":true,\"level\":3}}'\n"
              << "  axtpctl -t hid-sim --path /tmp/axtp-hid-audio.sock "
                 "-c audio.getAlgorithmConfig --json '{}'\n"
              << "  axtpctl -t hid --vid 0x1234 --pid 0x5678 -c audio.getAlgorithmConfig\n"
              << "  axtpctl -t tcp --host 127.0.0.1 --port 9000 -c audio.getAlgorithmConfig -o json\n";
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
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        if (std::isspace(static_cast<unsigned char>(ch))) {
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
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kDigits[(byte >> 4) & 0x0F]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::optional<std::uint32_t> parseUint32(const std::string& text) {
    std::size_t offset = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        offset = 2;
        base = 16;
    }
    try {
        const auto value = std::stoull(text.substr(offset), nullptr, base);
        if (value <= 0xFFFFFFFFULL) {
            return static_cast<std::uint32_t>(value);
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
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
            arg == "--vid" || arg == "--pid" || arg == "--path") {
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
            } else if (arg == "--path") {
                options->path = *value;
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

std::uint16_t readU16Le(const axtp::Bytes& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
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

    boost::json::object object;
    object["magic"] =
        ((*bytes)[0] == axtp::kAxtpStandardMagic0 && (*bytes)[1] == axtp::kAxtpStandardMagic1)
            ? "AX"
            : "invalid";
    object["version"] = (*bytes)[2];
    object["payloadType"] = payloadTypeName((*bytes)[3]);
    const auto payloadLength = readU16Le(*bytes, 4);
    object["payloadLength"] = payloadLength;
    object["sourceId"] = (*bytes)[6];
    object["destinationId"] = (*bytes)[7];
    object["messageId"] = readU16Le(*bytes, 8);
    object["frameIndex"] = (*bytes)[10];
    object["frameCount"] = (*bytes)[11];

    const auto total = static_cast<std::size_t>(axtp::kStandardFrameHeaderSize) + payloadLength +
                       axtp::kStandardFrameCrcSize;
    object["complete"] = bytes->size() >= total;
    if (bytes->size() >= total) {
        const auto expected = readU16Le(*bytes, total - axtp::kStandardFrameCrcSize);
        const auto actual =
            axtp::crc16CcittFalse(bytes->data(), total - axtp::kStandardFrameCrcSize);
        object["crcExpected"] = expected;
        object["crcActual"] = actual;
        object["crcOk"] = expected == actual;
    }

    std::cout << boost::json::serialize(object) << "\n";
    return 0;
}

boost::json::value parseJsonValueOrString(const axtp::Bytes& bytes) {
    if (bytes.empty()) {
        return nullptr;
    }
    try {
        const std::string text(bytes.begin(), bytes.end());
        return boost::json::parse(text);
    } catch (const std::exception&) {
        const std::string text(bytes.begin(), bytes.end());
        return boost::json::value(boost::json::string(text));
    }
}

bool validateJson(std::string_view text) {
    try {
        boost::json::parse(text);
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

void printJsonObject(const boost::json::object& object, OutputFormat format) {
    if (format == OutputFormat::Pretty) {
        std::cout << boost::json::serialize(object) << "\n";
        return;
    }
    std::cout << boost::json::serialize(object) << "\n";
}

std::string errorName(axtp::ErrorCode code) {
    const auto* descriptor = axtp::RegistryLookup::errorByCode(code);
    if (descriptor != nullptr) {
        return descriptor->name;
    }
    return "ERROR";
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
}

bool attachTransport(const CliOptions& options, axtp::sdk::AxtpClient* client) {
    if (options.transport == "mock") {
        client->attachTransport(std::make_unique<axtp::MockTransport>());
        return client->isConnected();
    }
    if (options.transport == "tcp") {
        client->attachTransport(std::make_unique<axtp::TcpTransport>(
            static_cast<std::uint16_t>(options.port.value_or(0)), options.host.c_str()));
        return client->isConnected();
    }
    if (options.transport == "websocket" || options.transport == "ws") {
        client->attachTransport(std::make_unique<axtp::WebSocketTransport>(
            static_cast<std::uint16_t>(options.port.value_or(0)), options.host.c_str()));
        return client->isConnected();
    }
    if (options.transport == "hid") {
        axtp::HidTransportOptions endpoint;
        endpoint.vendorId = static_cast<std::uint16_t>(options.vid.value_or(0));
        endpoint.productId = static_cast<std::uint16_t>(options.pid.value_or(0));
        endpoint.serialNumber = options.path;
        client->attachTransport(std::make_unique<axtp::HidTransport>(std::move(endpoint)));
        return client->isConnected();
    }
    if (options.transport == "hid-sim" || options.transport == "hidsim") {
        const auto path =
            !options.path.empty()
                ? options.path
                : (!options.endpoint.empty() ? options.endpoint : "/tmp/axtp-hid-audio.sock");
        axtp::HidTransportOptions endpoint;
        client->attachTransport(std::make_unique<axtp::HidTransport>(
            std::move(endpoint), axtp::LocalHidBackend::client(path)));
        return client->isConnected();
    }

    std::cerr << "unsupported transport: " << options.transport << "\n";
    return false;
}

axtp::RpcEncoding encodingFromName(const std::string& name) {
    if (name == "tlv") {
        return axtp::RpcEncoding::Tlv;
    }
    if (name == "raw") {
        return axtp::RpcEncoding::Raw;
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
        *encoding = axtp::RpcEncoding::Tlv;
        return true;
    }
    if (const auto file = optionValue(args, "--tlv-file")) {
        const auto contents = readBinaryFile(*file);
        if (!contents.has_value()) {
            std::cerr << "failed to read tlv file: " << *file << "\n";
            return false;
        }
        *body = *contents;
        *encoding = axtp::RpcEncoding::Tlv;
        return true;
    }
    if (const auto hex = optionValue(args, "--raw-hex")) {
        auto parsed = parseHex(*hex);
        if (!parsed.has_value()) {
            std::cerr << "invalid --raw-hex\n";
            return false;
        }
        *body = std::move(*parsed);
        *encoding = axtp::RpcEncoding::Raw;
        return true;
    }
    if (const auto file = optionValue(args, "--raw-file")) {
        const auto contents = readBinaryFile(*file);
        if (!contents.has_value()) {
            std::cerr << "failed to read raw file: " << *file << "\n";
            return false;
        }
        *body = *contents;
        *encoding = axtp::RpcEncoding::Raw;
        return true;
    }
    return true;
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

    axtp::sdk::AxtpClient client;
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

    if (!attachTransport(options, &client)) {
        std::cerr << "failed to connect transport: " << options.transport << "\n";
        return 4;
    }
    if (options.transport == "mock") {
        client.registerMethod(*methodId,
                              [](const axtp::RpcPayload& request) { return request.body; });
        installMockHandlers(client);
    }

    axtp::RpcPayload request;
    request.encoding = encoding;
    request.op = axtp::RpcOp::Request;
    request.methodOrEventId = *methodId;
    request.bodyEncoding =
        encoding == axtp::RpcEncoding::Tlv || encoding == axtp::RpcEncoding::Binary
            ? axtp::RpcBodyEncoding::Tlv8
            : axtp::RpcBodyEncoding::RawBytes;
    if (methodName.has_value()) {
        request.meta.jsonMethodOrEventName = *methodName;
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

    boost::json::object output;
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
        boost::json::object error;
        error["code"] = errorName(response.statusCode);
        error["numericCode"] = static_cast<std::uint16_t>(response.statusCode);
        error["message"] = errorName(response.statusCode);
        output["error"] = std::move(error);
    }
    printJsonObject(output, outputFormat);
    return response.statusCode == axtp::ErrorCode::Success ? 0 : 4;
}

int printCapabilityMethods() {
    boost::json::array methods;
    for (const auto& method : axtp::kMethodRegistry) {
        boost::json::object item;
        item["id"] = method.id;
        item["name"] = method.name;
        item["domain"] = method.domain;
        item["requestSchema"] = method.request_schema;
        item["responseSchema"] = method.response_schema;
        methods.push_back(std::move(item));
    }
    std::cout << boost::json::serialize(methods) << "\n";
    return 0;
}

int ping(const CliOptions& options) {
    boost::json::object output;
    output["ok"] = options.transport == "mock";
    output["transport"] = options.transport;
    output["wire"] = options.wire;
    if (options.transport != "mock") {
        output["message"] = "real transport ping is not implemented in P0";
    }
    std::cout << boost::json::serialize(output) << "\n";
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
