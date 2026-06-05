#pragma once

#include <boost/json.hpp>
#include <string>
#include <string_view>

#include "axtp_sdk_all.hpp"

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
        return boost::json::serialize(_config);
    }

    std::string setAlgorithmConfig(std::string_view params) {
        try {
            const auto parsed = boost::json::parse(params);
            if (parsed.is_object()) {
                const auto& root = parsed.as_object();
                if (const auto* config = root.if_contains("config");
                    config != nullptr && config->is_object()) {
                    mergeConfig(config->as_object());
                } else {
                    mergeConfig(root);
                }
            }
        } catch (const std::exception&) {
        }

        boost::json::object response;
        response["applyState"] = "applied";
        response["requiresAudioRestart"] = false;
        response["config"] = _config;
        return boost::json::serialize(response);
    }

private:
    static boost::json::object defaultConfig() {
        boost::json::object noiseSuppression;
        noiseSuppression["enabled"] = true;
        noiseSuppression["level"] = 1;

        boost::json::object echoCancellation;
        echoCancellation["enabled"] = true;
        echoCancellation["nlpLevel"] = 1;

        boost::json::object config;
        config["noiseSuppression"] = std::move(noiseSuppression);
        config["echoCancellation"] = std::move(echoCancellation);
        return config;
    }

    static std::string capabilitiesJson() {
        boost::json::object enabled;
        enabled["type"] = "boolean";
        enabled["defaultBoolean"] = true;

        boost::json::object level;
        level["type"] = "uint8";
        level["min"] = 0;
        level["max"] = 5;
        level["step"] = 1;
        level["defaultUInt8"] = 1;

        boost::json::object noiseSuppression;
        noiseSuppression["supported"] = true;
        noiseSuppression["enabled"] = enabled;
        noiseSuppression["level"] = level;

        boost::json::object nlpLevel = level;
        nlpLevel["max"] = 3;

        boost::json::object echoCancellation;
        echoCancellation["supported"] = true;
        echoCancellation["enabled"] = enabled;
        echoCancellation["nlpLevel"] = nlpLevel;

        boost::json::object algorithms;
        algorithms["noiseSuppression"] = std::move(noiseSuppression);
        algorithms["echoCancellation"] = std::move(echoCancellation);

        boost::json::object updatePolicy;
        updatePolicy["partialUpdateSupported"] = true;
        updatePolicy["multiAlgorithmUpdateSupported"] = true;
        updatePolicy["atomicUpdateSupported"] = true;

        boost::json::object response;
        response["capability"] = "audio.algorithm";
        response["updatePolicy"] = std::move(updatePolicy);
        response["algorithms"] = std::move(algorithms);
        return boost::json::serialize(response);
    }

    void mergeConfig(const boost::json::object& patch) {
        for (const auto& algorithm : patch) {
            if (!algorithm.value().is_object()) {
                _config[algorithm.key()] = algorithm.value();
                continue;
            }

            auto* currentValue = _config.if_contains(algorithm.key());
            if (currentValue == nullptr || !currentValue->is_object()) {
                _config[algorithm.key()] = algorithm.value();
                continue;
            }

            auto& current = currentValue->as_object();
            for (const auto& field : algorithm.value().as_object()) {
                current[field.key()] = field.value();
            }
        }
    }

    boost::json::object _config;
};

inline void installAudioDemoHandlers(sdk::AxtpServer& server, AudioDemoState& state) {
    state.install(server);
}

}  // namespace axtp::demo
