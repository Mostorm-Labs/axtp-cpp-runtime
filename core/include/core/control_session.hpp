#pragma once

#include <optional>
#include <utility>

#include "model/payload.hpp"

namespace axtp {

class ControlSession {
public:
    std::optional<ControlPayload> handle(ControlPayload payload) {
        _lastOpcode = payload.opcode;
        if (payload.opcode == ControlOpcode::Open) {
            _open = true;
            return makeResponse(ControlOpcode::Accept, payload);
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
    ControlOpcode _lastOpcode = ControlOpcode::Open;
};

}  // namespace axtp
