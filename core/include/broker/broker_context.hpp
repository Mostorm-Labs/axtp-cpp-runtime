#pragma once

#include <cstdint>
#include <string>

#include "model/payload.hpp"

namespace axtp {

enum class BrokerTaskType {
    RpcRequest,
    RpcEvent,
    StreamOpen,
    StreamData,
    StreamClose,
    ControlNotice,
};

enum class BrokerPriority {
    High,
    Normal,
    Low,
};

struct BrokerContext {
    std::uint32_t sessionId = 0;
    std::uint32_t requestId = 0;
    std::uint32_t methodOrEventId = 0;
    RpcEncoding encoding = RpcEncoding::Json;
    SourceProtocol sourceProtocol = SourceProtocol::AxtpV1;
    std::string traceId;
    std::uint64_t receivedAtMs = 0;
    std::uint64_t deadlineMs = 0;
};

}  // namespace axtp
