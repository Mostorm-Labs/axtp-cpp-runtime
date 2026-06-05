#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "audio_demo_handlers.hpp"
#include "axtp_sdk_all.hpp"
#include "hidapi/hid_local_backend.hpp"
#include "hidapi/hid_transport.hpp"

namespace {

volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
}

struct Options {
    std::string path = "/tmp/axtp-hid-audio.sock";
    std::uint8_t reportId = 0;
    std::size_t reportSize = 64;
    bool help = false;
};

void printUsage() {
    std::cout << "Usage: axtp_hid_audio_device_demo [--path SOCKET]\n"
              << "       [--report-id ID] [--report-size BYTES]\n";
}

bool parseUnsigned(const std::string& text, std::uint32_t* output) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stoul(text, &consumed, 0);
        if (consumed == text.size()) {
            *output = static_cast<std::uint32_t>(value);
            return true;
        }
    } catch (const std::exception&) {
    }
    return false;
}

bool parseOptions(int argc, char** argv, Options* options) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto requireValue = [&](const char* name) -> const char* {
            if (index + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++index];
        };

        if (arg == "--help" || arg == "-h") {
            options->help = true;
            return true;
        }
        if (arg == "--path") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            options->path = value;
            continue;
        }
        if (arg == "--report-id" || arg == "--report-size") {
            const auto* value = requireValue(arg.c_str());
            if (value == nullptr) {
                return false;
            }
            std::uint32_t parsed = 0;
            if (!parseUnsigned(value, &parsed)) {
                std::cerr << "invalid " << arg << "\n";
                return false;
            }
            if (arg == "--report-id") {
                if (parsed > 0xFF) {
                    std::cerr << "invalid --report-id\n";
                    return false;
                }
                options->reportId = static_cast<std::uint8_t>(parsed);
            } else {
                if (parsed < 2) {
                    std::cerr << "invalid --report-size\n";
                    return false;
                }
                options->reportSize = parsed;
            }
            continue;
        }

        std::cerr << "unknown option: " << arg << "\n";
        printUsage();
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, &options)) {
        return 2;
    }
    if (options.help) {
        printUsage();
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    axtp::HidTransportOptions hidOptions;
    hidOptions.reportId = options.reportId;
    hidOptions.inputReportSize = options.reportSize;
    hidOptions.outputReportSize = options.reportSize;

    axtp::sdk::AxtpServer server;
    axtp::demo::AudioDemoState state;
    axtp::demo::installAudioDemoHandlers(server, state);
    server.attachTransport(std::make_unique<axtp::HidTransport>(
        hidOptions, axtp::LocalHidBackend::server(options.path)));

    std::cout << "AXTP HID audio device demo listening at " << options.path << "\n";
    while (g_running != 0) {
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    server.close();
    return 0;
}
