#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/axtp_core.hpp"
#include "core/outbound/json_rpc_encoder.hpp"
#include "io/byte_sink.hpp"
#include "runtime/axtp_endpoint.hpp"
#include "transport/transport.hpp"

namespace axtp {

class WebSocketJsonRpcAdapter : public IByteSink {
public:
    template <typename Broker>
    WebSocketJsonRpcAdapter(AxtpEndpoint<Broker>& endpoint, ITransport& writer)
        : _core(&endpoint.core())
        , _writer(writer)
        , _pollEndpoint([&endpoint] { endpoint.poll(); }) {
        _core->configure(jsonRpcProfile(writer.profile()));
    }

    template <typename WebSocketLike>
    void poll(WebSocketLike& transport) {
        const bool hadConnection = transport.hasConnection();
        transport.poll();
        if (!hadConnection && transport.hasConnection()) {
            _helloSent = false;
            _identified = false;
            _sid = makeSessionId();
        }
        if (transport.hasConnection()) {
            sendHelloOnce();
        }
    }

    void onBytes(const Byte* data, std::size_t size) override {
        try {
            const std::string text(reinterpret_cast<const char*>(data), size);
            const auto parsed = boost::json::parse(text);
            const auto& object = parsed.as_object();
            const auto op = parseOp(object);

            if (op == RpcOp::Identify || op == RpcOp::Reidentify) {
                handleIdentify(object);
                return;
            }
            if (!_identified && (op == RpcOp::Request || op == RpcOp::RequestBatch)) {
                sendError(
                    parseSid(object), parseRequestId(object), ErrorCode::ControlOpenRequired, op);
                return;
            }
            if (op == RpcOp::RequestBatch) {
                sendError(
                    parseSid(object), parseRequestId(object), ErrorCode::RpcBatchUnsupported, op);
                return;
            }

            _core->byteSink().onBytes(data, size);
            _pollEndpoint();
        } catch (const std::exception&) {
            sendError("", 0, ErrorCode::RpcPayloadInvalid, RpcOp::Request);
        }
    }

    void sendRpc(RpcPayload payload) {
        auto bytes = _encoder.encode(std::move(payload));
        _writer.sendBytes(bytes.data(), bytes.size());
    }

    void sendEvent(RpcPayload payload) {
        payload.encoding = RpcEncoding::Json;
        payload.op = RpcOp::Event;
        payload.meta.sourceProtocol = SourceProtocol::JsonRpc;
        sendRpc(std::move(payload));
    }

private:
    static RpcOp parseOp(const boost::json::object& object) {
        const auto raw = object.at("op").as_int64();
        if (raw < 0 || raw > std::numeric_limits<std::uint8_t>::max()) {
            throw std::invalid_argument("invalid op");
        }
        return static_cast<RpcOp>(static_cast<std::uint8_t>(raw));
    }

    static TransportProfile jsonRpcProfile(TransportProfile profile) {
        profile.wireMode = AxtpWireMode::WebSocketJsonRpc;
        profile.defaultRpcEncoding = RpcEncoding::Json;
        profile.messageOriented = true;
        profile.supportsTextMessage = true;
        profile.supportsBinaryMessage = false;
        return profile;
    }

    static std::string parseSid(const boost::json::object& object) {
        if (!object.contains("sid") || !object.at("sid").is_string()) {
            return "";
        }
        return std::string(object.at("sid").as_string());
    }

    static std::uint32_t parseRequestId(const boost::json::object& object) {
        if (!object.contains("d") || !object.at("d").is_object()) {
            return 0;
        }
        const auto& d = object.at("d").as_object();
        if (!d.contains("id")) {
            return 0;
        }
        const auto& id = d.at("id");
        if (id.is_uint64()) {
            const auto raw = id.as_uint64();
            return raw <= std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(raw)
                       : 0;
        }
        if (id.is_int64()) {
            const auto raw = id.as_int64();
            return raw > 0 && raw <= std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(raw)
                       : 0;
        }
        return 0;
    }

    void sendHelloOnce() {
        if (_helloSent) {
            return;
        }
        sendRpc(JsonRpcEncoder::makeHello());
        _helloSent = true;
    }

    void handleIdentify(const boost::json::object& object) {
        const auto& d = object.at("d").as_object();
        if (const auto* resumeSid = d.if_contains("resumeSid");
            resumeSid != nullptr && resumeSid->is_string() && !resumeSid->as_string().empty()) {
            _sid = std::string(resumeSid->as_string());
        }
        _identified = true;
        sendRpc(JsonRpcEncoder::makeIdentified(_sid));
    }

    void
    sendError(const std::string& sid, std::uint32_t requestId, ErrorCode code, RpcOp requestOp) {
        RpcPayload response;
        response.encoding = RpcEncoding::Json;
        response.op =
            requestOp == RpcOp::RequestBatch ? RpcOp::RequestBatchResponse : RpcOp::RequestResponse;
        response.requestId = requestId;
        response.statusCode = code;
        response.bodyEncoding = RpcBodyEncoding::RawBytes;
        response.meta.sourceProtocol = SourceProtocol::JsonRpc;
        response.meta.jsonSid = sid.empty() ? _sid : sid;
        sendRpc(std::move(response));
    }

    std::string makeSessionId() {
        return std::to_string(_nextSessionId++);
    }

    AxtpCore* _core = nullptr;
    ITransport& _writer;
    std::function<void()> _pollEndpoint;
    JsonRpcEncoder _encoder;
    bool _helloSent = false;
    bool _identified = false;
    std::uint32_t _nextSessionId = 1;
    std::string _sid = makeSessionId();
};

}  // namespace axtp
