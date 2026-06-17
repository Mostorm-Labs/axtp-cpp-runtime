#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "core/control_tlv_codec.hpp"
#include "model/payload.hpp"

namespace axtp {

class ControlSession {
public:
    ControlPayload makeOpen(std::uint16_t controlId) {
        _open = false;
        _pendingOpenId = controlId;
        ControlPayload payload;
        payload.opcode = ControlOpcode::Open;
        payload.controlId = controlId;
        payload.statusCode = ErrorCode::Success;
        payload.tlv = ControlTlvCodec::defaultsForOpen();
        payload.body = ControlTlvCodec::encode(payload.tlv, false);
        return payload;
    }

    std::optional<ControlPayload> handle(ControlPayload payload) {
        _lastOpcode = payload.opcode;
        if (payload.opcode == ControlOpcode::Open) {
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
        if (payload.opcode == ControlOpcode::Close) {
            _open = false;
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
    ControlOpcode _lastOpcode = ControlOpcode::Open;
};

}  // namespace axtp
