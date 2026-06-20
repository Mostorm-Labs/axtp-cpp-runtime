#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include <axtp_sdk.hpp>
#include <core/runtime/testing/mock_transport.hpp>

int main() {
    axtp::sdk::AxtpClient client;
    client.attachTransport(std::make_unique<axtp::MockTransport>());

    client.registerMethod(
        static_cast<std::uint16_t>(axtp::MethodId::AudioGetAlgorithmConfig),
        [](const axtp::RpcPayload&) {
            const std::string body =
                R"({"noiseSuppression":{"enabled":true,"level":3}})";
            return axtp::Bytes(body.begin(), body.end());
        });

    const std::string response = client.callJson("audio.getAlgorithmConfig", "{}");
    std::cout << response << "\n";
    return 0;
}
