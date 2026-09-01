#pragma once

#include <cstdint>
#include <string>

#include "core/protocol/model/bytes.hpp"
#include "core/protocol/model/endpoint_metadata.hpp"
#include "core/protocol/model/protocol_types.hpp"

namespace axtp {

struct PayloadMeta {
    SourceProtocol sourceProtocol = SourceProtocol::AxtpV1;
    std::uint32_t sessionId = 0;
    std::uint32_t requestId = 0;
    bool hasRandomSeed = false;
    std::uint32_t randomSeed = 0;
    std::string jsonSid;
    std::string jsonMethodOrEventName;
    std::string jsonEventMasks;
    // Internal provenance only; encoders never serialize this bit.  It
    // distinguishes a decoder-synthesized protocol error that Core must send
    // back from a genuine inbound response that should be matched or dropped.
    bool localGeneratedResponse = false;
    // Internal ingress provenance only; encoders never serialize this value.
    // A host may provide a monotonic binding token at the raw payload ingress
    // boundary so deferred broker callbacks can reject data that crossed a
    // logical lease handoff while it was still queued in Core.
    std::uint64_t ingressToken = 0;
    // Opaque transport-local reply target captured at RPC ingress. It is never
    // serialized and remains distinct from Endpoint metadata, which is a wire
    // routing field rather than a connection-local delivery address.
    std::uint64_t replyTarget = 0;
    EndpointMetadata endpoint;
};

struct ControlTlvOptions {
    bool valid = true;
    bool hasSessionId = false;
    bool hasProtocolVersion = false;
    bool hasMaxFrameSize = false;
    bool hasMtu = false;
    bool hasSupportedPayloadTypes = false;
    bool hasSupportedRpcEncodings = false;
    bool hasSelectedRpcEncoding = false;
    bool hasHeartbeatIntervalMs = false;
    bool hasAckMode = false;
    std::uint32_t sessionId = 0;
    std::uint8_t protocolVersion = 1;
    std::uint32_t maxFrameSize = 4096;
    std::uint32_t mtu = 4096;
    std::uint8_t supportedPayloadTypes = 0x07;
    std::uint8_t supportedRpcEncodings = 0x09;
    std::uint8_t selectedRpcEncoding = static_cast<std::uint8_t>(RpcEncoding::Json);
    std::uint32_t heartbeatIntervalMs = 1000;
    std::uint8_t ackMode = 0;
};

struct ControlPayload {
    ControlOpcode opcode = ControlOpcode::Open;
    std::uint16_t controlId = 0;
    ErrorCode statusCode = ErrorCode::Success;
    PayloadMeta meta;
    ControlTlvOptions tlv;
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
