#include <cassert>
#include <string_view>
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

    broker.registerMethod(0x0901, [](const axtp::RpcPayload& request) {
        assert(request.methodOrEventId == 0x0901);
        return axtp::Bytes{0x77};
    });

    axtp::RpcPayload request;
    request.encoding = axtp::RpcEncoding::Tlv;
    request.op = axtp::RpcOp::Request;
    request.requestId = 900;
    request.methodOrEventId = 0x0901;
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
    assert(sink.rpcs[0].requestId == 900);
    assert(sink.rpcs[0].op == axtp::RpcOp::RequestResponse);
    assert((sink.rpcs[0].body == axtp::Bytes{0x77}));

    {
        axtp::BasicBroker<> dynamicBroker;
        dynamicBroker.registry().addMethod(0x90010001, "vendor.echo");
        dynamicBroker.registerJsonMethod(
            "audio.getAlgorithmConfig",
            [](const axtp::RpcContext& context, std::string_view params) {
                assert(context.methodName == "audio.getAlgorithmConfig");
                assert(params == "{}");
                return std::string(R"({"ok":true})");
            });
        dynamicBroker.registerTlvMethod(
            "audio.setAlgorithmConfig",
            [](const axtp::RpcContext& context, const axtp::Bytes& body) {
                assert(context.methodId == 0x0902);
                assert((body == axtp::Bytes{0x01, 0x01, 0x50}));
                return axtp::Bytes{0x02, 0x01, 0x01};
            });
        dynamicBroker.registerRawMethod(
            0x90010001, [](const axtp::RpcContext& context, const axtp::RpcRequestView& request) {
                assert(context.methodName == "vendor.echo");
                axtp::RpcResponseData response;
                response.body = request.body;
                return response;
            });

        axtp::BrokerTask jsonTask;
        jsonTask.type = axtp::BrokerTaskType::RpcRequest;
        jsonTask.rpc.encoding = axtp::RpcEncoding::Json;
        jsonTask.rpc.op = axtp::RpcOp::Request;
        jsonTask.rpc.requestId = 1001;
        jsonTask.rpc.methodOrEventId = 0x0901;
        jsonTask.rpc.bodyEncoding = axtp::RpcBodyEncoding::RawBytes;
        jsonTask.rpc.body = {'{', '}'};
        dynamicBroker.submit(std::move(jsonTask));

        axtp::BrokerTask tlvTask;
        tlvTask.type = axtp::BrokerTaskType::RpcRequest;
        tlvTask.rpc.encoding = axtp::RpcEncoding::Tlv;
        tlvTask.rpc.op = axtp::RpcOp::Request;
        tlvTask.rpc.requestId = 1002;
        tlvTask.rpc.methodOrEventId = 0x0902;
        tlvTask.rpc.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        tlvTask.rpc.body = {0x01, 0x01, 0x50};
        dynamicBroker.submit(std::move(tlvTask));

        axtp::BrokerTask rawTask;
        rawTask.type = axtp::BrokerTaskType::RpcRequest;
        rawTask.rpc.encoding = axtp::RpcEncoding::Raw;
        rawTask.rpc.op = axtp::RpcOp::Request;
        rawTask.rpc.requestId = 1003;
        rawTask.rpc.methodOrEventId = 0x90010001;
        rawTask.rpc.bodyEncoding = axtp::RpcBodyEncoding::RawBytes;
        rawTask.rpc.body = {0xDE, 0xAD};
        dynamicBroker.submit(std::move(rawTask));

        dynamicBroker.poll(3);
        auto jsonResult = dynamicBroker.pollResult();
        auto tlvResult = dynamicBroker.pollResult();
        auto rawResult = dynamicBroker.pollResult();
        assert(jsonResult.has_value());
        assert(tlvResult.has_value());
        assert(rawResult.has_value());
        assert(!dynamicBroker.pollResult().has_value());
        assert(jsonResult->type == axtp::BrokerResultType::RpcResponse);
        assert(tlvResult->type == axtp::BrokerResultType::RpcResponse);
        assert(rawResult->type == axtp::BrokerResultType::RpcResponse);
        assert(jsonResult->rpc.encoding == axtp::RpcEncoding::Json);
        assert((jsonResult->rpc.body ==
                axtp::Bytes{'{', '"', 'o', 'k', '"', ':', 't', 'r', 'u', 'e', '}'}));
        assert(tlvResult->rpc.encoding == axtp::RpcEncoding::Tlv);
        assert((tlvResult->rpc.body == axtp::Bytes{0x02, 0x01, 0x01}));
        assert(rawResult->rpc.encoding == axtp::RpcEncoding::Raw);
        assert((rawResult->rpc.body == axtp::Bytes{0xDE, 0xAD}));
    }

    return 0;
}
