#pragma once

#include "generated/method_traits.h"
#include "axtp_client.hpp"

namespace axtp::sdk {

class AudioClient {
public:
    explicit AudioClient(AxtpClient& client)
        : client_(client) {}

    AudioAlgorithmConfig getAlgorithmConfig(const AudioGetAlgorithmConfigRequest& request = {},
                                            CallOptions options = {}) {
        return client_.call<MethodId::AudioGetAlgorithmConfig>(request, options);
    }

    AudioGetAlgorithmCapabilitiesResponse
    getAlgorithmCapabilities(const AudioGetAlgorithmCapabilitiesRequest& request = {},
                             CallOptions options = {}) {
        return client_.call<MethodId::AudioGetAlgorithmCapabilities>(request, options);
    }

    AudioSetAlgorithmConfigResponse setAlgorithmConfig(const AudioSetAlgorithmConfigRequest& request,
                                                       CallOptions options = {}) {
        return client_.call<MethodId::AudioSetAlgorithmConfig>(request, options);
    }

    AudioSetAlgorithmConfigResponse resetAlgorithmConfig(const AudioResetAlgorithmConfigRequest& request,
                                                         CallOptions options = {}) {
        return client_.call<MethodId::AudioResetAlgorithmConfig>(request, options);
    }

private:
    AxtpClient& client_;
};

} // namespace axtp::sdk
