#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "core/protocol/wire/framed_binary/control_tlv_codec.hpp"
#include "core/protocol/model/payload.hpp"

namespace axtp {

class ControlSession {
public:
    static constexpr std::uint32_t kMinHeartbeatIntervalMs = 500;
    static constexpr std::uint32_t kMaxHeartbeatIntervalMs = 60000;

    void setRequestedHeartbeatInterval(std::uint32_t intervalMs) {
        _requestedHeartbeatIntervalMs = intervalMs < kMinHeartbeatIntervalMs
            ? kMinHeartbeatIntervalMs
            : (intervalMs > kMaxHeartbeatIntervalMs
                ? kMaxHeartbeatIntervalMs : intervalMs);
    }

    ControlPayload makeOpen(std::uint16_t controlId) {
        _open = false;
        _pendingOpenId = controlId;
        // A new OPEN starts a fresh negotiation.  Do not let a previously
        // accepted heartbeat interval survive a renegotiation in which the
        // peer omits (or invalidates) the interval field; callers must then
        // fall back to their legacy probe policy instead of probing at a
        // stale value from the prior physical/session handshake.
        _acceptedOptions = ControlTlvOptions{};
        ControlPayload payload;
        payload.opcode = ControlOpcode::Open;
        payload.controlId = controlId;
        payload.statusCode = ErrorCode::Success;
        payload.tlv = ControlTlvCodec::defaultsForOpen();
        payload.tlv.heartbeatIntervalMs = _requestedHeartbeatIntervalMs;
        payload.body = ControlTlvCodec::encode(payload.tlv, false);
        return payload;
    }

    // Construct a protocol heartbeat.  The caller owns the non-zero control
    // id and is responsible for polling until the matching ACK arrives.
    ControlPayload makeHeartbeat(std::uint16_t controlId) const {
        ControlPayload payload;
        payload.opcode = ControlOpcode::Heartbeat;
        payload.controlId = controlId;
        payload.statusCode = ErrorCode::Success;
        return payload;
    }

    std::optional<ControlPayload> handle(ControlPayload payload) {
        _lastOpcode = payload.opcode;
        if (payload.opcode == ControlOpcode::Open) {
            // The peer may reuse this Core for a fresh physical handshake.
            // Treat every OPEN as a new negotiation on both client and server
            // roles, so no prior ACCEPT capability leaks into the new session.
            _acceptedOptions = ControlTlvOptions{};
            auto response = makeResponse(ControlOpcode::Accept, payload);
            response.tlv = ControlTlvCodec::defaultsForAccept(payload.tlv);
            if (!payload.tlv.valid || !response.tlv.valid) {
                _open = false;
                response.statusCode = payload.tlv.valid ? ErrorCode::ControlNegotiationFailed
                                                        : ErrorCode::ControlPayloadInvalid;
            } else {
                _open = true;
            }
            response.body = ControlTlvCodec::encode(response.tlv, true);
            return response;
        }
        if (payload.opcode == ControlOpcode::Accept) {
            if (_pendingOpenId.has_value() && payload.controlId == *_pendingOpenId &&
                payload.statusCode == ErrorCode::Success && payload.tlv.valid) {
                _open = true;
                _acceptedOptions = payload.tlv;
                _pendingOpenId.reset();
            }
            return std::nullopt;
        }
        if (payload.opcode == ControlOpcode::Ping) {
            return makeResponse(ControlOpcode::Pong, payload);
        }
        if (payload.opcode == ControlOpcode::Heartbeat) {
            return makeResponse(ControlOpcode::HeartbeatAck, payload);
        }
        if (payload.opcode == ControlOpcode::Close) {
            _open = false;
            _pendingOpenId.reset();
            _acceptedOptions = ControlTlvOptions{};
            return makeResponse(ControlOpcode::CloseAck, payload);
        }
        return std::nullopt;
    }

    bool isOpen() const {
        return _open;
    }

    ControlOpcode lastOpcode() const {
        return _lastOpcode;
    }

    std::optional<std::uint16_t> pendingOpenId() const {
        return _pendingOpenId;
    }

    const ControlTlvOptions& acceptedOptions() const {
        return _acceptedOptions;
    }

    std::optional<std::uint32_t> negotiatedHeartbeatIntervalMs() const {
        if (!_acceptedOptions.hasHeartbeatIntervalMs ||
            _acceptedOptions.heartbeatIntervalMs < kMinHeartbeatIntervalMs ||
            _acceptedOptions.heartbeatIntervalMs > kMaxHeartbeatIntervalMs) {
            return std::nullopt;
        }
        return _acceptedOptions.heartbeatIntervalMs;
    }

private:
    static ControlPayload makeResponse(ControlOpcode opcode, const ControlPayload& request) {
        ControlPayload response;
        response.opcode = opcode;
        response.controlId = request.controlId;
        response.statusCode = ErrorCode::Success;
        response.meta = request.meta;
        return response;
    }

    bool _open = false;
    std::optional<std::uint16_t> _pendingOpenId;
    ControlTlvOptions _acceptedOptions;
    std::uint32_t _requestedHeartbeatIntervalMs = 1000;
    ControlOpcode _lastOpcode = ControlOpcode::Open;
};

}  // namespace axtp
