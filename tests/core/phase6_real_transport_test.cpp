#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <regex>
#include <string>
#include <thread>
#include <utility>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketMessageType.h>

#include "core/runtime/broker/basic_broker.hpp"
#include "core/runtime/core/axtp_core.hpp"
#include "core/protocol/wire/inbound_processor.hpp"
#include "core/protocol/wire/websocket_json_rpc/inbound/json_rpc_decoder.hpp"
#include "core/protocol/wire/outbound_processor.hpp"
#include "core/support/io/byte_writer_sink.hpp"
#include "json_rpc/websocket_json_rpc_adapter.hpp"
#include "core/runtime/endpoint/axtp_endpoint.hpp"
#include "core/runtime/testing/mock_transport.hpp"
#include "transports/tcp/native/tcp_transport.hpp"
#include "transports/websocket/ix/websocket_transport.hpp"

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

struct CapturingByteSink : axtp::IByteSink {
    axtp::Bytes bytes;

    void onBytes(const axtp::Byte* data, std::size_t size) override {
        bytes.insert(bytes.end(), data, data + size);
    }
};

axtp::Bytes encodeRpcRequest(std::uint32_t requestId) {
    axtp::RpcPayload request;
    request.encoding = axtp::jsonBinaryRpcEncoding();
    request.op = axtp::RpcOp::Request;
    request.requestId = requestId;
    request.methodOrEventId = 0x0901;
    request.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
    CapturingByteWriter writer;
    axtp::OutboundProcessor outbound(writer);
    outbound.sendRpcRequest(request);
    return writer.bytes;
}

void injectJson(axtp::MockTransport& transport, const std::string& text) {
    transport.injectIncoming(axtp::Bytes(text.begin(), text.end()));
}

nlohmann::json popJson(axtp::MockTransport& transport, const char* label) {
    auto bytes = transport.tryPopOutgoing();
    if (!bytes.has_value()) {
        std::cerr << "missing JSON output: " << label << '\n';
        assert(bytes.has_value());
    }
    const std::string text(bytes->begin(), bytes->end());
    return nlohmann::json::parse(text);
}

std::string jsonString(const nlohmann::json& object, const char* key) {
    return object.at(key).get<std::string>();
}

struct WebSocketProbe {
    explicit WebSocketProbe(std::uint16_t port) {
        ws.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/");
        ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
            if (!message || message->type != ix::WebSocketMessageType::Message) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                messages.push(message->str);
            }
            cv.notify_one();
        });
        ws.start();
    }

    ~WebSocketProbe() {
        ws.stop();
    }

    std::string waitMessage() {
        std::unique_lock<std::mutex> lock(mutex);
        const auto ok = cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return !messages.empty();
        });
        assert(ok);
        auto text = std::move(messages.front());
        messages.pop();
        return text;
    }

    void sendText(const std::string& text) {
        ws.sendText(text);
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::string> messages;
    ix::WebSocket ws;
};

}  // namespace

int main() {
    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        broker.registerMethod(0x0901, [](const axtp::RpcPayload&) { return axtp::Bytes{0xA1}; });

        axtp::TcpServerTransport server(0);
        endpoint.attachTransport(server);
        server.open();
        assert(server.profile().kind == axtp::TransportKind::Tcp);
        const auto port = server.localPort();
        assert(port != 0);

        CapturingByteSink clientSink;
        axtp::TcpClientTransport client("127.0.0.1", port, std::chrono::milliseconds(500));
        client.bind(clientSink);
        client.open();
        assert(client.isOpen());
        const auto first = encodeRpcRequest(601);
        const auto second = encodeRpcRequest(602);
        client.sendBytes(first.data(), 5);
        client.sendBytes(first.data() + 5, first.size() - 5);
        client.sendBytes(second.data(), second.size());

        for (int i = 0; i < 100 && clientSink.bytes.empty(); ++i) {
            server.poll();
            endpoint.poll();
            client.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(!clientSink.bytes.empty());
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(clientSink.bytes.data(), clientSink.bytes.size());
        assert(!sink.rpcs.empty());
        assert(sink.rpcs[0].op == axtp::RpcOp::RequestResponse);
        assert(sink.rpcs[0].requestId == 601);
        assert((sink.rpcs[0].body == axtp::Bytes{0xA1}));
        client.close();
        server.close();
    }

    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        broker.registerMethod(0x0901, [](const axtp::RpcPayload& request) {
            assert(request.meta.sourceProtocol == axtp::SourceProtocol::JsonRpc);
            assert(request.meta.jsonMethodOrEventName == "audio.getAlgorithmConfig");
            const std::string result = R"({"noiseSuppression":{"enabled":true,"level":3}})";
            return axtp::Bytes(result.begin(), result.end());
        });
        broker.registerMethod(0x0902, [](const axtp::RpcPayload& request) {
            const std::string params(request.body.begin(), request.body.end());
            assert(params == R"({"noiseSuppression":{"enabled":true,"level":3}})");
            return axtp::Bytes{};
        });
        broker.registerMethod(0x090D, [](const axtp::RpcPayload&) { return axtp::Bytes{0xB1}; });

        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        axtp::WebSocketJsonRpcAdapter adapter(endpoint, transport);
        transport.bind(adapter);
        transport.open();

        injectJson(transport,
                   R"({"sid":"","op":7,"d":{"id":700,"method":"audio.getAlgorithmConfig"}})");
        auto beforeIdentify = popJson(transport, "before identify");
        assert(beforeIdentify.at("op").get<int>() ==
               static_cast<int>(axtp::RpcOp::RequestResponse));
        assert(beforeIdentify.at("d").at("status").at("code").get<int>() ==
               static_cast<int>(axtp::ErrorCode::ControlOpenRequired));

        injectJson(transport, R"({"sid":"","op":2,"d":{"resumeSid":"legacy-session"}})");
        auto legacyIdentified = popJson(transport, "legacy identify without randomSeed");
        assert(legacyIdentified.at("op").get<int>() ==
               static_cast<int>(axtp::RpcOp::Identified));
        assert(legacyIdentified.at("sid").get<std::string>() == "legacy-session");

        injectJson(
            transport,
            R"({"sid":"legacy-session","op":7,"d":{"id":706,"method":"audio.getAlgorithmConfig","params":{}}})");
        auto legacyResponse = popJson(transport, "legacy request without randomSeed");
        auto legacyD = legacyResponse.at("d");
        assert(legacyD.at("id").get<int>() == 706);
        assert(legacyD.at("status").at("ok").get<bool>());
        assert(legacyD.at("result").contains("noiseSuppression"));

        injectJson(transport,
                   R"({"sid":"","op":2,"d":{"randomSeed":305419896,"eventMasks":"850101"}})");
        auto identified = popJson(transport, "identified");
        assert(identified.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Identified));
        const auto sid = jsonString(identified, "sid");
        assert(std::regex_match(sid, std::regex("^[0-9A-F]{8}$")));
        assert(sid != "00000000");
        assert(sid != "12345678");

        injectJson(transport,
                   R"({"sid":")" + sid +
                       R"(","op":7,"d":{"id":701,"method":"audio.getAlgorithmConfig","params":{}}})");
        auto response = popJson(transport, "audio.getAlgorithmConfig");
        assert(response.at("sid").get<std::string>() == sid);
        assert(response.at("op").get<int>() == static_cast<int>(axtp::RpcOp::RequestResponse));
        auto d = response.at("d");
        assert(d.at("id").get<int>() == 701);
        assert(d.at("status").at("ok").get<bool>());
        assert(d.at("result").contains("noiseSuppression"));

        injectJson(
            transport,
            R"({"sid":")" + sid +
                R"(","op":7,"d":{"id":702,"method":"audio.setAlgorithmConfig","params":{"noiseSuppression":{"enabled":true,"level":3}}}})");
        response = popJson(transport, "audio.setAlgorithmConfig");
        d = response.at("d");
        assert(d.at("id").get<int>() == 702);
        assert(d.at("status").at("ok").get<bool>());
        assert(!d.contains("result"));

        injectJson(transport,
                   R"({"sid":")" + sid +
                       R"(","op":7,"d":{"id":703,"method":"audio.unknown","params":{}}})");
        response = popJson(transport, "unknown method");
        d = response.at("d");
        assert(d.at("status").at("code").get<int>() ==
               static_cast<int>(axtp::ErrorCode::RpcMethodNotFound));

        injectJson(transport,
                   R"({"sid":")" + sid +
                       R"(","op":7,"d":{"id":704,"method":"audio.getAlgorithmCapabilities","params":{}}})");
        response = popJson(transport, "invalid JSON response body");
        d = response.at("d");
        assert(d.at("status").at("code").get<int>() ==
               static_cast<int>(axtp::ErrorCode::RpcBodyDecodeFailed));
        assert(!d.contains("result"));

        injectJson(transport, R"({"sid":")" + sid + R"(","op":9,"d":{"id":705,"requests":[]}})");
        response = popJson(transport, "batch unsupported");
        d = response.at("d");
        assert(response.at("op").get<int>() ==
               static_cast<int>(axtp::RpcOp::RequestBatchResponse));
        assert(d.at("status").at("code").get<int>() ==
               static_cast<int>(axtp::ErrorCode::RpcBatchUnsupported));

        injectJson(
            transport,
            R"({"sid":"0x000003","op":7,"d":{"id":708,"method":"audio.getAlgorithmConfig","params":{}}})");
        response = popJson(transport, "invalid sid");
        d = response.at("d");
        assert(response.at("op").get<int>() == static_cast<int>(axtp::RpcOp::RequestResponse));
        assert(d.at("id").get<int>() == 708);
        assert(d.at("status").at("ok").get<bool>() == false);
        assert(d.at("status").at("code").get<int>() ==
               static_cast<int>(axtp::ErrorCode::RpcPayloadInvalid));
        assert(!d.contains("result"));

        axtp::RpcPayload event;
        event.op = axtp::RpcOp::Event;
        event.methodOrEventId = 0x0901;
        event.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
        event.meta.jsonSid = sid;
        const std::string eventData = R"({"reason":"manual","applyState":"applied"})";
        event.body = axtp::Bytes(eventData.begin(), eventData.end());
        adapter.sendEvent(std::move(event));
        auto eventJson = popJson(transport, "event");
        assert(eventJson.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Event));
        auto eventD = eventJson.at("d");
        assert(eventD.at("event").get<std::string>() == "audio.algorithmConfigChanged");
        assert(eventD.at("data").at("reason").get<std::string>() == "manual");
    }

    {
        CapturingPayloadSink sink;
        axtp::JsonRpcDecoder inbound(sink);
        const std::string text =
            R"({"sid":"json-session","op":7,"d":{"id":901,"method":"audio.getAlgorithmConfig","params":{}}})";
        inbound.onBytes(reinterpret_cast<const axtp::Byte*>(text.data()), text.size());
        assert(sink.rpcs.size() == 1);
        assert(sink.rpcs[0].op == axtp::RpcOp::Request);
        assert(sink.rpcs[0].requestId == 901);
        assert(sink.rpcs[0].methodOrEventId == 0x0901);
        assert(sink.rpcs[0].meta.sourceProtocol == axtp::SourceProtocol::JsonRpc);
    }

    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        broker.registerMethod(0x0901, [](const axtp::RpcPayload&) {
            const std::string result = R"({"ok":true})";
            return axtp::Bytes(result.begin(), result.end());
        });

        axtp::MockTransport transport;
        endpoint.attachTransport(transport);
        axtp::WebSocketJsonRpcAdapter adapter(endpoint, transport);
        transport.bind(adapter);
        transport.open();

        injectJson(transport,
                   R"({"sid":"","op":2,"d":{"randomSeed":305419896,"resumeSid":"legacy-session"}})");
        auto identified = popJson(transport, "legacy identified");
        assert(identified.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Identified));
        assert(identified.at("sid").get<std::string>() == "legacy-session");

        injectJson(
            transport,
            R"({"sid":"legacy-session","op":7,"d":{"id":707,"method":"audio.getAlgorithmConfig","params":{}}})");
        auto response = popJson(transport, "legacy sid request");
        const auto d = response.at("d");
        assert(d.at("id").get<int>() == 707);
        assert(d.at("status").at("ok").get<bool>());
        assert(d.at("result").at("ok").get<bool>());
    }

    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        broker.registerMethod(0x0901, [](const axtp::RpcPayload&) {
            const std::string result = R"({"ok":true})";
            return axtp::Bytes(result.begin(), result.end());
        });

        axtp::WebSocketTransport server(0);
        endpoint.attachTransport(server);
        axtp::WebSocketJsonRpcAdapter adapter(endpoint, server);
        server.bind(adapter);
        server.open();
        assert(server.profile().kind == axtp::TransportKind::WebSocket);
        assert(server.profile().wireMode == axtp::AxtpWireMode::WebSocketJsonRpc);
        const auto port = server.localPort();
        assert(port != 0);

        std::atomic<bool> clientDone{false};
        std::string helloText;
        std::string identifiedText;
        std::string responseText;
        std::thread clientThread([&] {
            std::mutex mutex;
            std::condition_variable cv;
            std::queue<std::string> messages;
            ix::WebSocket ws;
            ws.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/");
            ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& message) {
                if (!message || message->type != ix::WebSocketMessageType::Message) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    messages.push(message->str);
                }
                cv.notify_one();
            });
            ws.start();

            auto waitMessage = [&]() {
                std::unique_lock<std::mutex> lock(mutex);
                const auto ok = cv.wait_for(lock, std::chrono::seconds(5), [&] {
                    return !messages.empty();
                });
                assert(ok);
                auto text = std::move(messages.front());
                messages.pop();
                return text;
            };

            helloText = waitMessage();
            ws.sendText(R"({"sid":"","op":2,"d":{"randomSeed":305419896}})");
            identifiedText = waitMessage();
            const auto identified = nlohmann::json::parse(identifiedText);
            const auto sid = identified.at("sid").get<std::string>();
            ws.sendText(std::string(R"({"sid":")" + sid +
                                    R"(","op":7,"d":{"id":801,"method":"audio.getAlgorithmConfig","params":{}}})"));
            responseText = waitMessage();
            ws.stop();
            clientDone = true;
        });

        for (int i = 0; i < 2000 && !clientDone; ++i) {
            adapter.poll(server);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        clientThread.join();
        assert(clientDone);
        auto hello = nlohmann::json::parse(helloText);
        assert(hello.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Hello));
        auto identified = nlohmann::json::parse(identifiedText);
        assert(identified.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Identified));
        auto parsed = nlohmann::json::parse(responseText);
        assert(parsed.at("op").get<int>() == static_cast<int>(axtp::RpcOp::RequestResponse));
        const auto& d = parsed.at("d");
        assert(d.at("id").get<int>() == 801);
        assert(d.at("status").at("ok").get<bool>());
        assert(d.at("result").at("ok").get<bool>());
        server.close();
    }

    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        broker.registerMethod(0x0901, [](const axtp::RpcPayload&) {
            const std::string result = R"({"ok":true})";
            return axtp::Bytes(result.begin(), result.end());
        });

        axtp::WebSocketTransport server(0);
        endpoint.attachTransport(server);
        axtp::WebSocketJsonRpcAdapter adapter(endpoint, server);
        server.bind(adapter);
        server.open();
        const auto port = server.localPort();
        assert(port != 0);

        std::atomic<bool> keepPolling{true};
        std::thread poller([&] {
            while (keepPolling.load()) {
                adapter.poll(server);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        WebSocketProbe first(port);
        auto firstHello = nlohmann::json::parse(first.waitMessage());
        assert(firstHello.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Hello));
        first.sendText(R"({"sid":"","op":2,"d":{"randomSeed":305419896}})");
        auto firstIdentified = nlohmann::json::parse(first.waitMessage());
        assert(firstIdentified.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Identified));
        const auto firstSid = firstIdentified.at("sid").get<std::string>();
        assert(std::regex_match(firstSid, std::regex("^[0-9A-F]{8}$")));
        assert(firstSid != "00000000");
        assert(firstSid != "12345678");

        WebSocketProbe second(port);
        auto secondHello = nlohmann::json::parse(second.waitMessage());
        assert(secondHello.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Hello));
        second.sendText(R"({"sid":"","op":2,"d":{"randomSeed":305419897}})");
        auto secondIdentified = nlohmann::json::parse(second.waitMessage());
        assert(secondIdentified.at("op").get<int>() == static_cast<int>(axtp::RpcOp::Identified));
        const auto secondSid = secondIdentified.at("sid").get<std::string>();
        assert(std::regex_match(secondSid, std::regex("^[0-9A-F]{8}$")));
        assert(secondSid != "00000000");
        assert(secondSid != "12345679");
        assert(secondSid != firstSid);

        keepPolling = false;
        poller.join();
        server.close();
    }

    return 0;
}
