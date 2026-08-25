#include <cassert>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/runtime/broker/basic_broker.hpp"
#include "core/runtime/core/axtp_core.hpp"
#include "core/protocol/wire/inbound_processor.hpp"
#include "core/protocol/wire/outbound_processor.hpp"
#include "core/support/io/byte_writer_sink.hpp"
#include "core/runtime/endpoint/axtp_endpoint.hpp"
#include "core/runtime/testing/mock_transport.hpp"

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
    request.encoding = axtp::jsonBinaryRpcEncoding();
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
                assert(context.endpoint.src == "ep_app");
                assert(context.endpoint.dst == "ep_device");
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
        jsonTask.rpc.bodyEncoding = axtp::RpcBodyEncoding::None;
        jsonTask.rpc.meta.endpoint.src = "ep_app";
        jsonTask.rpc.meta.endpoint.dst = "ep_device";
        jsonTask.rpc.body = {'{', '}'};
        dynamicBroker.submit(std::move(jsonTask));

        axtp::BrokerTask tlvTask;
        tlvTask.type = axtp::BrokerTaskType::RpcRequest;
        tlvTask.rpc.encoding = axtp::jsonBinaryRpcEncoding();
        tlvTask.rpc.op = axtp::RpcOp::Request;
        tlvTask.rpc.requestId = 1002;
        tlvTask.rpc.methodOrEventId = 0x0902;
        tlvTask.rpc.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
        tlvTask.rpc.body = {0x01, 0x01, 0x50};
        dynamicBroker.submit(std::move(tlvTask));

        axtp::BrokerTask rawTask;
        rawTask.type = axtp::BrokerTaskType::RpcRequest;
        rawTask.rpc.encoding = axtp::jsonBinaryRpcEncoding();
        rawTask.rpc.op = axtp::RpcOp::Request;
        rawTask.rpc.requestId = 1003;
        rawTask.rpc.methodOrEventId = 0x90010001;
        rawTask.rpc.bodyEncoding = axtp::RpcBodyEncoding::None;
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
        assert(jsonResult->rpc.meta.endpoint.src == "ep_device");
        assert(jsonResult->rpc.meta.endpoint.dst == "ep_app");
        assert((jsonResult->rpc.body ==
                axtp::Bytes{'{', '"', 'o', 'k', '"', ':', 't', 'r', 'u', 'e', '}'}));
        assert(tlvResult->rpc.encoding == axtp::jsonBinaryRpcEncoding());
        assert((tlvResult->rpc.body == axtp::Bytes{0x02, 0x01, 0x01}));
        assert(rawResult->rpc.encoding == axtp::jsonBinaryRpcEncoding());
        assert((rawResult->rpc.body == axtp::Bytes{0xDE, 0xAD}));
    }

    {
        axtp::BasicBroker<> compatibilityBroker;
        compatibilityBroker.registerMethod(0x0901, [](const axtp::RpcPayload& request) {
            assert(request.meta.endpoint.src == "ep_app");
            assert(request.meta.endpoint.dst == "ep_device");
            return axtp::Bytes{'{', '}'};
        });

        axtp::BrokerTask task;
        task.type = axtp::BrokerTaskType::RpcRequest;
        task.rpc.encoding = axtp::RpcEncoding::Json;
        task.rpc.op = axtp::RpcOp::Request;
        task.rpc.requestId = 1004;
        task.rpc.methodOrEventId = 0x0901;
        task.rpc.meta.endpoint.src = "ep_app";
        task.rpc.meta.endpoint.dst = "ep_device";
        compatibilityBroker.submit(std::move(task));
        compatibilityBroker.poll();
        const auto result = compatibilityBroker.pollResult();
        assert(result.has_value());
        assert(result->rpc.meta.endpoint.src == "ep_device");
        assert(result->rpc.meta.endpoint.dst == "ep_app");
    }

    {
        axtp::BasicBroker<> deferredBroker;
        bool firstReady = false;

        deferredBroker.registerDeferredRawMethod(
            0xA001,
            [&firstReady](const axtp::RpcContext&, const axtp::RpcRequestView&) {
                return [&firstReady]() -> std::optional<axtp::RpcResponseData> {
                    if (!firstReady) {
                        return std::nullopt;
                    }
                    return axtp::RpcResponseData{axtp::RpcEncoding::Json, {'A'}};
                };
            });
        deferredBroker.registerDeferredRawMethod(
            0xA002,
            [](const axtp::RpcContext&, const axtp::RpcRequestView&) {
                return []() -> std::optional<axtp::RpcResponseData> {
                    return axtp::RpcResponseData{axtp::RpcEncoding::Json, {'B'}};
                };
            });

        axtp::BrokerTask blockedTask;
        blockedTask.type = axtp::BrokerTaskType::RpcRequest;
        blockedTask.rpc.requestId = 2001;
        blockedTask.rpc.methodOrEventId = 0xA001;
        blockedTask.rpc.meta.endpoint.src = "ep_app";
        blockedTask.rpc.meta.endpoint.dst = "ep_device_a";
        deferredBroker.submit(std::move(blockedTask));

        axtp::BrokerTask readyTask;
        readyTask.type = axtp::BrokerTaskType::RpcRequest;
        readyTask.rpc.requestId = 2002;
        readyTask.rpc.methodOrEventId = 0xA002;
        readyTask.rpc.meta.endpoint.src = "ep_app";
        readyTask.rpc.meta.endpoint.dst = "ep_device_b";
        deferredBroker.submit(std::move(readyTask));

        deferredBroker.poll(2);
        assert(!deferredBroker.pollResult().has_value());

        deferredBroker.poll();
        const auto readyResult = deferredBroker.pollResult();
        assert(readyResult.has_value());
        assert(readyResult->type == axtp::BrokerResultType::RpcResponse);
        assert(readyResult->rpc.requestId == 2002);
        assert((readyResult->rpc.body == axtp::Bytes{'B'}));
        assert(readyResult->rpc.meta.endpoint.src == "ep_device_b");
        assert(readyResult->rpc.meta.endpoint.dst == "ep_app");
        assert(!deferredBroker.pollResult().has_value());

        firstReady = true;
        deferredBroker.poll();
        const auto blockedResult = deferredBroker.pollResult();
        assert(blockedResult.has_value());
        assert(blockedResult->type == axtp::BrokerResultType::RpcResponse);
        assert(blockedResult->rpc.requestId == 2001);
        assert((blockedResult->rpc.body == axtp::Bytes{'A'}));
        assert(blockedResult->rpc.meta.endpoint.src == "ep_device_a");
        assert(blockedResult->rpc.meta.endpoint.dst == "ep_app");
    }

    {
        axtp::BasicBroker<> deferredErrorBroker;
        deferredErrorBroker.registerDeferredRawMethod(
            0xA003,
            [](const axtp::RpcContext&, const axtp::RpcRequestView&) {
                return []() -> std::optional<axtp::RpcResponseData> {
                    return axtp::RpcResponseData{axtp::RpcEncoding::Json,
                                                  {},
                                                  false,
                                                  axtp::ErrorCode::InvalidArgument,
                                                  true};
                };
            });
        deferredErrorBroker.registerDeferredRawMethod(
            0xA004,
            [](const axtp::RpcContext&, const axtp::RpcRequestView&) {
                return []() -> std::optional<axtp::RpcResponseData> {
                    throw 1;
                };
            });
        deferredErrorBroker.registerDeferredRawMethod(
            0xA005,
            [](const axtp::RpcContext&, const axtp::RpcRequestView&) {
                return axtp::DeferredRpcPoll{};
            });

        for (const auto methodId : {0xA003U, 0xA004U, 0xA005U}) {
            axtp::BrokerTask task;
            task.type = axtp::BrokerTaskType::RpcRequest;
            task.rpc.requestId = methodId;
            task.rpc.methodOrEventId = methodId;
            deferredErrorBroker.submit(std::move(task));
        }
        deferredErrorBroker.poll(3);
        deferredErrorBroker.poll();

        std::vector<axtp::BrokerResult> errorResults;
        while (auto result = deferredErrorBroker.pollResult()) {
            errorResults.push_back(std::move(*result));
        }
        assert(errorResults.size() == 3);
        for (const auto& result : errorResults) {
            assert(result.type == axtp::BrokerResultType::RpcError);
            if (result.rpc.requestId == 0xA003U) {
                assert(result.rpc.statusCode == axtp::ErrorCode::InvalidArgument);
            } else {
                assert(result.rpc.requestId == 0xA004U || result.rpc.requestId == 0xA005U);
                assert(result.rpc.statusCode == axtp::ErrorCode::InternalError);
            }
        }
    }

    {
        int destroyedPolls = 0;
        std::weak_ptr<int> pendingLifetime;
        {
            axtp::BasicBroker<> pendingBroker;
            const auto lifetime = std::make_shared<int>(0);
            pendingLifetime = lifetime;
            pendingBroker.registerDeferredRawMethod(
                0xA006,
                [&destroyedPolls, lifetime](const axtp::RpcContext&,
                                             const axtp::RpcRequestView&) {
                    return [&destroyedPolls, lifetime]() -> std::optional<axtp::RpcResponseData> {
                        ++destroyedPolls;
                        return std::nullopt;
                    };
                });
            axtp::BrokerTask task;
            task.type = axtp::BrokerTaskType::RpcRequest;
            task.rpc.requestId = 0xA006;
            task.rpc.methodOrEventId = 0xA006;
            pendingBroker.submit(std::move(task));
            pendingBroker.poll();
            assert(destroyedPolls == 0);
            assert(!pendingLifetime.expired());
        }
        assert(destroyedPolls == 0);
        assert(pendingLifetime.expired());
    }

    return 0;
}
