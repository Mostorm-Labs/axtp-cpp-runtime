#include <cassert>
#include <utility>
#include <vector>

#include "broker/basic_broker.hpp"
#include "core/axtp_core.hpp"
#include "core/inbound/inbound_processor.hpp"
#include "core/outbound/outbound_processor.hpp"
#include "io/byte_writer_sink.hpp"
#include "runtime/axtp_endpoint.hpp"
#include "testing/mock_transport.hpp"

namespace {

struct CapturingByteWriter : axtp::IByteWriter {
    axtp::Bytes bytes;

    void writeBytes(const axtp::Byte* data, std::size_t size) override {
        bytes.insert(bytes.end(), data, data + size);
    }
};

struct CapturingPayloadSink : axtp::IPayloadSink {
    std::vector<axtp::RpcPayload> rpcs;

    void onControl(axtp::ControlPayload) override {}

    void onRpc(axtp::RpcPayload payload) override {
        rpcs.push_back(std::move(payload));
    }

    void onStream(axtp::StreamPayload) override {}
};

}  // namespace

int main() {
    axtp::BasicBroker<> broker;
    axtp::AxtpEndpoint endpoint(broker);
    axtp::MockTransport transport;
    endpoint.attachTransport(transport);
    transport.open();
    assert(transport.isOpen());
    assert(transport.profile().kind == axtp::TransportKind::Mock);

    broker.registerMethod(0x0101, [](const axtp::RpcPayload&) { return axtp::Bytes{0x42}; });

    axtp::RpcPayload request;
    request.encoding = axtp::RpcEncoding::Tlv;
    request.op = axtp::RpcOp::Request;
    request.requestId = 500;
    request.methodOrEventId = 0x0101;
    request.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;

    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendRpcRequest(request);
    transport.injectIncoming(writer.bytes);

    endpoint.poll();
    auto outgoing = transport.tryPopOutgoing();
    assert(outgoing.has_value());

    CapturingPayloadSink sink;
    axtp::InboundProcessor inbound(sink);
    inbound.onBytes(outgoing->data(), outgoing->size());
    assert(sink.rpcs.size() == 1);
    assert(sink.rpcs[0].op == axtp::RpcOp::RequestResponse);
    assert(sink.rpcs[0].requestId == 500);
    assert((sink.rpcs[0].body == axtp::Bytes{0x42}));

    return 0;
}
