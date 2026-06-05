#pragma once

#include <cstddef>
#include <vector>

#include "generated/axtp_method_registry_generated.h"

#include "axtp_client.hpp"

namespace axtp::sdk {

class CapabilityClient {
public:
    explicit CapabilityClient(AxtpClient& client) {
        (void)client;
    }

    std::vector<MethodDescriptor> methods() const {
        return std::vector<MethodDescriptor>(kMethodRegistry,
                                             kMethodRegistry + kMethodRegistryCount);
    }
};

}  // namespace axtp::sdk
