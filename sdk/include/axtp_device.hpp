#pragma once

#include "axtp_client.hpp"
#include "capability_client.hpp"
#include "generated/audio_client.h"

namespace axtp::sdk {

class AxtpDevice {
public:
    explicit AxtpDevice(AxtpClient& client)
        : audio(client)
        , capability(client)
    {}

    AudioClient audio;
    CapabilityClient capability;
};

}  // namespace axtp::sdk
