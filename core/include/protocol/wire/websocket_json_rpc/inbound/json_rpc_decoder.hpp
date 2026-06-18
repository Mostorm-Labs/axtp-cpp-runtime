#pragma once

#include "protocol/wire/websocket_json_rpc/inbound/json_rpc_payload_decoder.hpp"
#include "support/io/byte_sink.hpp"

namespace axtp {

class JsonRpcDecoder : public IByteSink {
public:
    explicit JsonRpcDecoder(IPayloadSink& sink)
        : _sink(sink) {}

    // WebSocketJsonRpc mode receives one complete WebSocket text message per call.
    // It is not a byte-stream parser and must not be fed arbitrary TCP chunks.
    void onBytes(const Byte* data, std::size_t size) override {
        JsonRpcPayloadDecoder::decode(data, size, _sink, SourceProtocol::JsonRpc);
    }

private:
    IPayloadSink& _sink;
};

}  // namespace axtp
