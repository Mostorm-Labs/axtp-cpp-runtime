#pragma once

#include "sdk/axtp_client.hpp"
#include "sdk/capability_client.hpp"
#include "sdk/generated/audio_client.h"

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
