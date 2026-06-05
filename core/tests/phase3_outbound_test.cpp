#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/inbound/inbound_processor.hpp"
#include "core/outbound/outbound_processor.hpp"
#include "io/byte_writer_sink.hpp"

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

}  // namespace

int main() {
    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        axtp::RpcPayload rpc;
        rpc.encoding = axtp::RpcEncoding::Tlv;
        rpc.op = axtp::RpcOp::Request;
        rpc.requestId = 42;
        rpc.methodOrEventId = 0x0101;
        rpc.statusCode = axtp::ErrorCode::Success;
        rpc.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        rpc.body = {0x01, 0x02, 0x03};
        outbound.sendRpcRequest(rpc);

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].requestId == 42);
        assert(sink.rpcs[0].methodOrEventId == 0x0101);
        assert((sink.rpcs[0].body == axtp::Bytes{0x01, 0x02, 0x03}));
    }

    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer, 24);
        axtp::RpcPayload rpc;
        rpc.encoding = axtp::RpcEncoding::Tlv;
        rpc.op = axtp::RpcOp::Request;
        rpc.requestId = 43;
        rpc.methodOrEventId = 0x0101;
        rpc.statusCode = axtp::ErrorCode::Success;
        rpc.bodyEncoding = axtp::RpcBodyEncoding::RawBytes;
        for (std::uint8_t value = 0; value < 40; ++value) {
            rpc.body.push_back(value);
        }
        outbound.sendRpcRequest(rpc);

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].requestId == 43);
        assert(sink.rpcs[0].body == rpc.body);
    }

    {
        CapturingByteWriter writer;
        axtp::OutboundProcessor outbound(writer);
        axtp::ControlPayload control;
        control.opcode = axtp::ControlOpcode::Open;
        control.controlId = 7;
        control.statusCode = axtp::ErrorCode::Success;
        control.body = {0x10};
        outbound.sendControl(control);

        axtp::StreamPayload stream;
        stream.streamId = 9;
        stream.seqId = 10;
        stream.cursor = 11;
        stream.data = {0x20, 0x21};
        outbound.sendStream(stream);

        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(writer.bytes.data(), writer.bytes.size());
        assert(sink.controls.size() == 1);
        assert(sink.controls[0].opcode == axtp::ControlOpcode::Open);
        assert(sink.controls[0].controlId == 7);
        assert((sink.controls[0].body == axtp::Bytes{0x10}));
        assert(sink.streams.size() == 1);
        assert(sink.streams[0].streamId == 9);
        assert(sink.streams[0].seqId == 10);
        assert(sink.streams[0].cursor == 11);
        assert((sink.streams[0].data == axtp::Bytes{0x20, 0x21}));
    }

    return 0;
}
