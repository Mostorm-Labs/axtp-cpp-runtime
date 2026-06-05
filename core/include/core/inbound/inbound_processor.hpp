#pragma once

#include "core/inbound/frame_decoder.hpp"
#include "core/inbound/json_rpc_decoder.hpp"
#include "core/inbound/message_reassembler.hpp"
#include "core/inbound/payload_decoder.hpp"
#include "io/byte_sink.hpp"
#include "transport/transport_profile.hpp"

namespace axtp {

class InboundProcessor : public IByteSink {
public:
    explicit InboundProcessor(IPayloadSink& sink)
        : _payloadDecoder(sink)
        , _messageReassembler(_payloadDecoder)
        , _frameDecoder(_messageReassembler)
        , _jsonRpcDecoder(sink) {}

    void onBytes(const Byte* data, std::size_t size) override {
        if (_wireMode == AxtpWireMode::WebSocketJsonRpc) {
            _jsonRpcDecoder.onBytes(data, size);
            return;
        }
        _frameDecoder.onBytes(data, size);
    }

    void setWireMode(AxtpWireMode wireMode) {
        _wireMode = wireMode;
    }

private:
    PayloadDecoder _payloadDecoder;
    MessageReassembler _messageReassembler;
    FrameDecoder _frameDecoder;
    JsonRpcDecoder _jsonRpcDecoder;
    AxtpWireMode _wireMode = AxtpWireMode::FramedBinary;
};

}  // namespace axtp
