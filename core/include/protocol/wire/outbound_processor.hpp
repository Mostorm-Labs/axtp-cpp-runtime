#pragma once

#include <cstddef>
#include <utility>

#include "protocol/wire/framed_binary/outbound/frame_encoder.hpp"
#include "protocol/wire/websocket_json_rpc/outbound/json_rpc_encoder.hpp"
#include "protocol/wire/framed_binary/outbound/message_fragmenter.hpp"
#include "protocol/wire/framed_binary/outbound/payload_encoder.hpp"
#include "support/io/byte_writer_sink.hpp"
#include "runtime/transport/transport_profile.hpp"

namespace axtp {

class OutboundProcessor {
public:
    explicit OutboundProcessor(IByteWriter& writer, std::size_t maxFrameSize = 4096)
        : _writer(writer)
        , _messageFragmenter(maxFrameSize) {}

    void sendControl(ControlPayload payload) {
        if (_wireMode == AxtpWireMode::WebSocketJsonRpc) {
            return;
        }
        sendMessage(_payloadEncoder.encodeControl(payload));
    }

    void sendRpcRequest(RpcPayload payload) {
        sendRpc(std::move(payload));
    }

    void sendRpcResponse(RpcPayload payload) {
        sendRpc(std::move(payload));
    }

    void sendRpcError(RpcPayload payload) {
        sendRpc(std::move(payload));
    }

    void sendEvent(RpcPayload payload) {
        sendRpc(std::move(payload));
    }

    void sendStream(StreamPayload payload) {
        if (_wireMode == AxtpWireMode::WebSocketJsonRpc) {
            return;
        }
        sendMessage(_payloadEncoder.encodeStream(payload));
    }

    void sendRpc(RpcPayload payload) {
        if (_wireMode == AxtpWireMode::WebSocketJsonRpc) {
            const auto bytes = _jsonRpcEncoder.encode(std::move(payload));
            _writer.writeBytes(bytes.data(), bytes.size());
            return;
        }
        sendMessage(_payloadEncoder.encodeRpc(payload));
    }

    void setWireMode(AxtpWireMode wireMode) {
        _wireMode = wireMode;
    }

    void setMaxFrameSize(std::size_t maxFrameSize) {
        _messageFragmenter.setMaxFrameSize(maxFrameSize);
    }

private:
    void sendMessage(Message message) {
        auto frames = _messageFragmenter.fragment(std::move(message));
        for (const auto& frame : frames) {
            auto bytes = _frameEncoder.encode(frame);
            _writer.writeBytes(bytes.data(), bytes.size());
        }
    }

    IByteWriter& _writer;
    PayloadEncoder _payloadEncoder;
    MessageFragmenter _messageFragmenter;
    FrameEncoder _frameEncoder;
    JsonRpcEncoder _jsonRpcEncoder;
    AxtpWireMode _wireMode = AxtpWireMode::FramedBinary;
};

}  // namespace axtp
