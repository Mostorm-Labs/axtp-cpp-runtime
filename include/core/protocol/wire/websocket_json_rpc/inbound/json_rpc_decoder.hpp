#pragma once

#include "core/protocol/wire/websocket_json_rpc/inbound/json_rpc_payload_decoder.hpp"
#include "core/support/io/byte_sink.hpp"

#include <utility>

namespace axtp {

class JsonRpcDecoder : public IByteSink {
public:
    using NameLookup = JsonRpcPayloadDecoder::NameLookup;

    explicit JsonRpcDecoder(IPayloadSink& sink)
        : _sink(sink) {}

    void setMethodLookup(NameLookup lookup) {
        _methodLookup = std::move(lookup);
    }

    void setEventLookup(NameLookup lookup) {
        _eventLookup = std::move(lookup);
    }

    // WebSocketJsonRpc mode receives one complete WebSocket text message per call.
    // It is not a byte-stream parser and must not be fed arbitrary TCP chunks.
    void onBytes(const Byte* data, std::size_t size) override {
        JsonRpcPayloadDecoder::decode(data, size, _sink, SourceProtocol::JsonRpc, _methodLookup, _eventLookup);
    }

private:
    IPayloadSink& _sink;
    NameLookup _methodLookup;
    NameLookup _eventLookup;
};

}  // namespace axtp
