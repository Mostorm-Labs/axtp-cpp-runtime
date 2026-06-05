#include <atomic>
#include <boost/json.hpp>
#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "audio_demo_handlers.hpp"
#include "axtp_sdk_all.hpp"
#include "hidapi/hid_local_backend.hpp"
#include "hidapi/hid_transport.hpp"

#if !defined(_WIN32)
#    include <unistd.h>
#endif

int main() {
#if defined(_WIN32)
    return 0;
#else
    const std::string path =
        std::string("/tmp/axtp-hid-audio-e2e-") + std::to_string(getpid()) + ".sock";

    axtp::HidTransportOptions options;
    options.inputReportSize = 64;
    options.outputReportSize = 64;
    options.maxReportsPerPoll = 64;

    axtp::sdk::AxtpServer server;
    axtp::demo::AudioDemoState state;
    axtp::demo::installAudioDemoHandlers(server, state);
    server.attachTransport(
        std::make_unique<axtp::HidTransport>(options, axtp::LocalHidBackend::server(path)));

    std::atomic<bool> running{true};
    std::thread serverThread([&]() {
        while (running.load()) {
            server.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    axtp::sdk::AxtpClient client;
    client.attachTransport(
        std::make_unique<axtp::HidTransport>(options, axtp::LocalHidBackend::client(path)));

    axtp::sdk::CallOptions callOptions;
    callOptions.timeout = std::chrono::seconds(1);

    const auto before = client.callJson("audio.getAlgorithmConfig", "{}", callOptions);
    assert(before.find("noiseSuppression") != std::string::npos);

    const auto setResponse =
        client.callJson("audio.setAlgorithmConfig",
                        R"({"config":{"noiseSuppression":{"enabled":true,"level":3}}})",
                        callOptions);
    const auto setJson = boost::json::parse(setResponse).as_object();
    assert(setJson.at("applyState").as_string() == "applied");
    assert(setJson.at("config")
               .as_object()
               .at("noiseSuppression")
               .as_object()
               .at("level")
               .as_int64() == 3);

    const auto after = client.callJson("audio.getAlgorithmConfig", "{}", callOptions);
    const auto afterJson = boost::json::parse(after).as_object();
    assert(afterJson.at("noiseSuppression").as_object().at("level").as_int64() == 3);

    client.close();
    running.store(false);
    serverThread.join();
    server.close();
    return 0;
#endif
}
