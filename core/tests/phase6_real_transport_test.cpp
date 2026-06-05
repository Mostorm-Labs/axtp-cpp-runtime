#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include "broker/basic_broker.hpp"
#include "core/axtp_core.hpp"
#include "core/inbound/inbound_processor.hpp"
#include "core/inbound/json_rpc_decoder.hpp"
#include "core/outbound/outbound_processor.hpp"
#include "io/byte_writer_sink.hpp"
#include "websocket_json_rpc_adapter.hpp"
#include "runtime/axtp_endpoint.hpp"
#include "testing/mock_transport.hpp"
#include "tcp_boost/tcp_transport.hpp"
#include "websocket_boost/websocket_transport.hpp"

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

axtp::Bytes encodeRpcRequest(std::uint32_t requestId) {
    axtp::RpcPayload request;
    request.encoding = axtp::RpcEncoding::Tlv;
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

boost::json::object popJson(axtp::MockTransport& transport, const char* label) {
    auto bytes = transport.tryPopOutgoing();
    if (!bytes.has_value()) {
        std::cerr << "missing JSON output: " << label << '\n';
        assert(bytes.has_value());
    }
    const std::string text(bytes->begin(), bytes->end());
    return boost::json::parse(text).as_object();
}

std::string jsonString(const boost::json::object& object, const char* key) {
    return std::string(object.at(key).as_string());
}

}  // namespace

int main() {
    {
        axtp::BasicBroker<> broker;
        axtp::AxtpEndpoint endpoint(broker);
        broker.registerMethod(0x0901, [](const axtp::RpcPayload&) { return axtp::Bytes{0xA1}; });

        axtp::TcpTransport server(0);
        endpoint.attachTransport(server);
        server.open();
        assert(server.profile().kind == axtp::TransportKind::Tcp);
        const auto port = server.localPort();
        assert(port != 0);

        boost::asio::io_context io;
        boost::asio::ip::tcp::socket client(io);
        client.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        client.non_blocking(true);
        const auto first = encodeRpcRequest(601);
        const auto second = encodeRpcRequest(602);
        boost::asio::write(client, boost::asio::buffer(first));
        boost::asio::write(client, boost::asio::buffer(second));

        axtp::Bytes responseBytes;
        for (int i = 0; i < 100 && responseBytes.empty(); ++i) {
            server.poll();
            endpoint.poll();
            std::array<axtp::Byte, 4096> buffer{};
            boost::system::error_code ec;
            const auto n = client.read_some(boost::asio::buffer(buffer), ec);
            if (!ec && n > 0) {
                responseBytes.insert(responseBytes.end(), buffer.begin(), buffer.begin() + n);
                break;
            }
            if (ec != boost::asio::error::would_block && ec != boost::asio::error::try_again) {
                assert(false);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(!responseBytes.empty());
        CapturingPayloadSink sink;
        axtp::InboundProcessor inbound(sink);
        inbound.onBytes(responseBytes.data(), responseBytes.size());
        assert(!sink.rpcs.empty());
        assert(sink.rpcs[0].op == axtp::RpcOp::RequestResponse);
        assert(sink.rpcs[0].requestId == 601);
        assert((sink.rpcs[0].body == axtp::Bytes{0xA1}));
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
        assert(beforeIdentify.at("op").as_int64() ==
               static_cast<int>(axtp::RpcOp::RequestResponse));
        assert(beforeIdentify.at("d").as_object().at("status").as_object().at("code").as_int64() ==
               static_cast<int>(axtp::ErrorCode::ControlOpenRequired));

        injectJson(transport, R"({"sid":"","op":2,"d":{"rpcVersion":1,"eventMasks":"850101"}})");
        auto identified = popJson(transport, "identified");
        assert(identified.at("op").as_int64() == static_cast<int>(axtp::RpcOp::Identified));
        const auto sid = jsonString(identified, "sid");
        assert(!sid.empty());

        injectJson(transport,
                   R"({"sid":")" + sid +
                       R"(","op":7,"d":{"id":701,"method":"audio.getAlgorithmConfig","params":{}}})");
        auto response = popJson(transport, "audio.getAlgorithmConfig");
        assert(response.at("sid").as_string() == sid);
        assert(response.at("op").as_int64() == static_cast<int>(axtp::RpcOp::RequestResponse));
        auto d = response.at("d").as_object();
        assert(d.at("id").as_int64() == 701);
        assert(d.at("status").as_object().at("ok").as_bool());
        assert(d.at("result").as_object().contains("noiseSuppression"));

        injectJson(
            transport,
            R"({"sid":")" + sid +
                R"(","op":7,"d":{"id":702,"method":"audio.setAlgorithmConfig","params":{"noiseSuppression":{"enabled":true,"level":3}}}})");
        response = popJson(transport, "audio.setAlgorithmConfig");
        d = response.at("d").as_object();
        assert(d.at("id").as_int64() == 702);
        assert(d.at("status").as_object().at("ok").as_bool());
        assert(!d.contains("result"));

        injectJson(transport,
                   R"({"sid":")" + sid +
                       R"(","op":7,"d":{"id":703,"method":"audio.unknown","params":{}}})");
        response = popJson(transport, "unknown method");
        d = response.at("d").as_object();
        assert(d.at("status").as_object().at("code").as_int64() ==
               static_cast<int>(axtp::ErrorCode::RpcMethodNotFound));

        injectJson(transport,
                   R"({"sid":")" + sid +
                       R"(","op":7,"d":{"id":704,"method":"audio.getAlgorithmCapabilities","params":{}}})");
        response = popJson(transport, "invalid JSON response body");
        d = response.at("d").as_object();
        assert(d.at("status").as_object().at("code").as_int64() ==
               static_cast<int>(axtp::ErrorCode::RpcBodyDecodeFailed));
        assert(!d.contains("result"));

        injectJson(transport, R"({"sid":")" + sid + R"(","op":9,"d":{"id":705,"requests":[]}})");
        response = popJson(transport, "batch unsupported");
        d = response.at("d").as_object();
        assert(response.at("op").as_int64() == static_cast<int>(axtp::RpcOp::RequestBatchResponse));
        assert(d.at("status").as_object().at("code").as_int64() ==
               static_cast<int>(axtp::ErrorCode::RpcBatchUnsupported));

        axtp::RpcPayload event;
        event.op = axtp::RpcOp::Event;
        event.methodOrEventId = 0x0901;
        event.meta.sourceProtocol = axtp::SourceProtocol::JsonRpc;
        event.meta.jsonSid = sid;
        const std::string eventData = R"({"reason":"manual","applyState":"applied"})";
        event.body = axtp::Bytes(eventData.begin(), eventData.end());
        adapter.sendEvent(std::move(event));
        auto eventJson = popJson(transport, "event");
        assert(eventJson.at("op").as_int64() == static_cast<int>(axtp::RpcOp::Event));
        auto eventD = eventJson.at("d").as_object();
        assert(eventD.at("event").as_string() == "audio.algorithmConfigChanged");
        assert(eventD.at("data").as_object().at("reason").as_string() == "manual");
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
            boost::asio::io_context io;
            boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws(io);
            boost::asio::ip::tcp::resolver resolver(io);
            auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
            boost::asio::connect(ws.next_layer(), endpoints);
            ws.handshake("127.0.0.1", "/");
            boost::beast::flat_buffer buffer;
            ws.read(buffer);
            helloText = boost::beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            ws.write(boost::asio::buffer(std::string(R"({"sid":"","op":2,"d":{"rpcVersion":1}})")));
            ws.read(buffer);
            identifiedText = boost::beast::buffers_to_string(buffer.data());
            auto identified = boost::json::parse(identifiedText).as_object();
            const auto sid = std::string(identified.at("sid").as_string());
            buffer.consume(buffer.size());
            ws.write(boost::asio::buffer(
                std::string(R"({"sid":")" + sid +
                            R"(","op":7,"d":{"id":801,"method":"audio.getAlgorithmConfig","params":{}}})")));
            ws.read(buffer);
            responseText = boost::beast::buffers_to_string(buffer.data());
            clientDone = true;
        });

        for (int i = 0; i < 100 && !clientDone; ++i) {
            adapter.poll(server);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        clientThread.join();
        assert(clientDone);
        auto hello = boost::json::parse(helloText).as_object();
        assert(hello.at("op").as_int64() == static_cast<int>(axtp::RpcOp::Hello));
        auto identified = boost::json::parse(identifiedText).as_object();
        assert(identified.at("op").as_int64() == static_cast<int>(axtp::RpcOp::Identified));
        auto parsed = boost::json::parse(responseText).as_object();
        assert(parsed.at("op").as_int64() == static_cast<int>(axtp::RpcOp::RequestResponse));
        const auto& d = parsed.at("d").as_object();
        assert(d.at("id").as_int64() == 801);
        assert(d.at("status").as_object().at("ok").as_bool());
        assert(d.at("result").as_object().at("ok").as_bool());
        server.close();
    }

    return 0;
}
