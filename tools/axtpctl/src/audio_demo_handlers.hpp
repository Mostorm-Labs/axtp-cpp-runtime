#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "sdk/axtp_sdk_all.hpp"

namespace axtp::demo {

class AudioDemoState {
public:
    AudioDemoState()
        : _config(defaultConfig()) {}

    void install(sdk::AxtpServer& server) {
        server.onJson("audio.getAlgorithmConfig",
                      [this](const RpcContext&, std::string_view) { return configJson(); });
        server.onJson("audio.getAlgorithmCapabilities",
                      [](const RpcContext&, std::string_view) { return capabilitiesJson(); });
        server.onJson("audio.setAlgorithmConfig",
                      [this](const RpcContext&, std::string_view params) {
                          return setAlgorithmConfig(params);
                      });
    }

    std::string configJson() const {
        return _config.dump();
    }

    std::string setAlgorithmConfig(std::string_view params) {
        try {
            const auto parsed = nlohmann::json::parse(params);
            if (parsed.is_object()) {
                if (const auto config = parsed.find("config");
                    config != parsed.end() && config->is_object()) {
                    mergeConfig(*config);
                } else {
                    mergeConfig(parsed);
                }
            }
        } catch (const std::exception&) {
        }

        auto response = nlohmann::json::object();
        response["applyState"] = "applied";
        response["requiresAudioRestart"] = false;
        response["config"] = _config;
        return response.dump();
    }

private:
    static nlohmann::json defaultConfig() {
        auto noiseSuppression = nlohmann::json::object();
        noiseSuppression["enabled"] = true;
        noiseSuppression["level"] = 1;

        auto echoCancellation = nlohmann::json::object();
        echoCancellation["enabled"] = true;
        echoCancellation["nlpLevel"] = 1;

        auto config = nlohmann::json::object();
        config["noiseSuppression"] = std::move(noiseSuppression);
        config["echoCancellation"] = std::move(echoCancellation);
        return config;
    }

    static std::string capabilitiesJson() {
        auto enabled = nlohmann::json::object();
        enabled["type"] = "boolean";
        enabled["defaultBoolean"] = true;

        auto level = nlohmann::json::object();
        level["type"] = "uint8";
        level["min"] = 0;
        level["max"] = 5;
        level["step"] = 1;
        level["defaultUInt8"] = 1;

        auto noiseSuppression = nlohmann::json::object();
        noiseSuppression["supported"] = true;
        noiseSuppression["enabled"] = enabled;
        noiseSuppression["level"] = level;

        auto nlpLevel = level;
        nlpLevel["max"] = 3;

        auto echoCancellation = nlohmann::json::object();
        echoCancellation["supported"] = true;
        echoCancellation["enabled"] = enabled;
        echoCancellation["nlpLevel"] = nlpLevel;

        auto algorithms = nlohmann::json::object();
        algorithms["noiseSuppression"] = std::move(noiseSuppression);
        algorithms["echoCancellation"] = std::move(echoCancellation);

        auto updatePolicy = nlohmann::json::object();
        updatePolicy["partialUpdateSupported"] = true;
        updatePolicy["multiAlgorithmUpdateSupported"] = true;
        updatePolicy["atomicUpdateSupported"] = true;

        auto response = nlohmann::json::object();
        response["capability"] = "audio.algorithm";
        response["updatePolicy"] = std::move(updatePolicy);
        response["algorithms"] = std::move(algorithms);
        return response.dump();
    }

    void mergeConfig(const nlohmann::json& patch) {
        for (auto algorithm = patch.begin(); algorithm != patch.end(); ++algorithm) {
            if (!algorithm.value().is_object()) {
                _config[algorithm.key()] = algorithm.value();
                continue;
            }

            auto currentValue = _config.find(algorithm.key());
            if (currentValue == _config.end() || !currentValue->is_object()) {
                _config[algorithm.key()] = algorithm.value();
                continue;
            }

            auto& current = *currentValue;
            for (auto field = algorithm.value().begin(); field != algorithm.value().end();
                 ++field) {
                current[field.key()] = field.value();
            }
        }
    }

    nlohmann::json _config;
};

inline void installAudioDemoHandlers(sdk::AxtpServer& server, AudioDemoState& state) {
    state.install(server);
}

}  // namespace axtp::demo
