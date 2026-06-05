#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "broker/basic_broker.hpp"
#include "core/axtp_core.hpp"
#include "core/inbound/inbound_processor.hpp"
#include "core/outbound/outbound_processor.hpp"
#include "io/byte_writer_sink.hpp"
#include "runtime/axtp_endpoint.hpp"

namespace {

struct CapturingByteWriter : axtp::IByteWriter {
    axtp::Bytes bytes;

    void writeBytes(const axtp::Byte* data, std::size_t size) override {
        bytes.insert(bytes.end(), data, data + size);
    }
};

struct CapturingPayloadSink : axtp::IPayloadSink {
    std::vector<axtp::ControlPayload> controls;
    std::vector<axtp::RpcPayload> rpcs;
    std::vector<axtp::StreamPayload> streams;

    void onControl(axtp::ControlPayload payload) override {
        controls.push_back(std::move(payload));
    }

    void onRpc(axtp::RpcPayload payload) override {
        rpcs.push_back(std::move(payload));
    }

    void onStream(axtp::StreamPayload payload) override {
        streams.push_back(std::move(payload));
    }
};

axtp::Bytes encodeRpc(axtp::RpcPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendRpcRequest(std::move(payload));
    return writer.bytes;
}

axtp::Bytes encodeControl(axtp::ControlPayload payload) {
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendControl(std::move(payload));
    return writer.bytes;
}

}  // namespace

int main() {
    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        broker.registerMethod(0x0101, [](const axtp::RpcPayload& request) {
            assert(request.requestId == 100);
            return axtp::Bytes{0x99, 0x88};
        });

        axtp::RpcPayload request;
        request.encoding = axtp::RpcEncoding::Tlv;
        request.op = axtp::RpcOp::Request;
        request.requestId = 100;
        request.methodOrEventId = 0x0101;
        request.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        auto requestBytes = encodeRpc(request);
        endpoint.core().byteSink().onBytes(requestBytes.data(), requestBytes.size());
        endpoint.poll();

        auto responseBytes = endpoint.core().tryPopOutboundBytes();
        assert(responseBytes.has_value());
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(responseBytes->data(), responseBytes->size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].op == axtp::RpcOp::RequestResponse);
        assert(sink.rpcs[0].requestId == 100);
        assert(sink.rpcs[0].methodOrEventId == 0x0101);
        assert((sink.rpcs[0].body == axtp::Bytes{0x99, 0x88}));
    }

    {
        axtp::AxtpCore core;
        axtp::ControlPayload open;
        open.opcode = axtp::ControlOpcode::Open;
        open.controlId = 1;
        auto bytes = encodeControl(open);
        core.byteSink().onBytes(bytes.data(), bytes.size());
        assert(core.controlSessionOpen());
        auto responseBytes = core.tryPopOutboundBytes();
        assert(responseBytes.has_value());
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(responseBytes->data(), responseBytes->size());
        assert(sink.controls.size() == 1);
        assert(sink.controls[0].opcode == axtp::ControlOpcode::Accept);
        assert(sink.controls[0].controlId == 1);

        axtp::ControlPayload ping;
        ping.opcode = axtp::ControlOpcode::Ping;
        ping.controlId = 2;
        bytes = encodeControl(ping);
        core.byteSink().onBytes(bytes.data(), bytes.size());
        responseBytes = core.tryPopOutboundBytes();
        assert(responseBytes.has_value());
        CapturingPayloadSink pingSink;
        axtp::InboundProcessor pingInbound(pingSink);
        pingInbound.onBytes(responseBytes->data(), responseBytes->size());
        assert(pingSink.controls.size() == 1);
        assert(pingSink.controls[0].opcode == axtp::ControlOpcode::Pong);
    }

    {
        axtp::AxtpCore core;
        core.expectRpcResponse(55);

        axtp::RpcPayload response;
        response.encoding = axtp::RpcEncoding::Tlv;
        response.op = axtp::RpcOp::RequestResponse;
        response.requestId = 55;
        response.methodOrEventId = 0x0101;
        response.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        response.body = {0x01};

        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        outbound.sendRpcResponse(response);
        core.byteSink().onBytes(writer.bytes.data(), writer.bytes.size());
        auto matched = core.tryTakeRpcResponse(55);
        assert(matched.has_value());
        assert((matched->body == axtp::Bytes{0x01}));
    }

    return 0;
}
