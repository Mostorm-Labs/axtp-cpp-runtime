#pragma once

#include "core/protocol/wire/framed_binary/inbound/frame_decoder.hpp"
#include "core/protocol/wire/websocket_json_rpc/inbound/json_rpc_decoder.hpp"
#include "core/protocol/wire/framed_binary/inbound/message_reassembler.hpp"
#include "core/protocol/wire/framed_binary/inbound/payload_decoder.hpp"
#include "core/support/io/byte_sink.hpp"
#include "core/runtime/transport/transport_profile.hpp"

#include <utility>

namespace axtp {

class InboundProcessor : public IByteSink {
public:
    using NameLookup = JsonRpcDecoder::NameLookup;

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

    void setJsonRpcMethodLookup(NameLookup lookup) {
        _jsonRpcDecoder.setMethodLookup(std::move(lookup));
    }

    void setJsonRpcEventLookup(NameLookup lookup) {
        _jsonRpcDecoder.setEventLookup(std::move(lookup));
    }

private:
    PayloadDecoder _payloadDecoder;
    MessageReassembler _messageReassembler;
    FrameDecoder _frameDecoder;
    JsonRpcDecoder _jsonRpcDecoder;
    AxtpWireMode _wireMode = AxtpWireMode::FramedBinary;
};

}  // namespace axtp
