#pragma once

#include <cstdint>
#include <string>

#include "model/bytes.hpp"
#include "model/protocol_types.hpp"

namespace axtp {

struct PayloadMeta {
    SourceProtocol sourceProtocol = SourceProtocol::AxtpV1;
    std::uint32_t sessionId = 0;
    std::uint32_t requestId = 0;
    std::string jsonSid;
    std::string jsonMethodOrEventName;
};

struct ControlPayload {
    ControlOpcode opcode = ControlOpcode::Open;
    std::uint16_t controlId = 0;
    ErrorCode statusCode = ErrorCode::Success;
    PayloadMeta meta;
    Bytes body;
};

struct RpcPayload {
    RpcEncoding encoding = RpcEncoding::Json;
    RpcOp op = RpcOp::Request;
    std::uint32_t requestId = 0;
    std::uint32_t methodOrEventId = 0;
    ErrorCode statusCode = ErrorCode::Success;
    RpcBodyEncoding bodyEncoding = RpcBodyEncoding::Tlv8;
    PayloadMeta meta;
    Bytes body;
};

struct StreamPayload {
    std::uint32_t streamId = 0;
    std::uint32_t seqId = 0;
    std::uint64_t cursor = 0;
    PayloadMeta meta;
    Bytes data;
};

}  // namespace axtp
