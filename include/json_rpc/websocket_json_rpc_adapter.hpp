#pragma once

#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/runtime/core/axtp_core.hpp"
#include "core/protocol/wire/websocket_json_rpc/outbound/json_rpc_encoder.hpp"
#include "core/support/io/byte_sink.hpp"
#include "core/runtime/endpoint/axtp_endpoint.hpp"
#include "core/runtime/transport/transport.hpp"

namespace axtp {

class WebSocketJsonRpcAdapter : public IByteSink {
public:
    template <typename Broker>
    WebSocketJsonRpcAdapter(AxtpEndpoint<Broker>& endpoint, ITransport& writer)
        : _core(&endpoint.core())
        , _writer(writer)
        , _pollEndpoint([&endpoint] { endpoint.poll(); }) {
        _core->setJsonRpcMethodLookup([&endpoint](std::string_view methodName) {
            return endpoint.broker().registry().findMethodId(methodName);
        });
        _core->configure(jsonRpcProfile(writer.profile()));
    }

    template <typename WebSocketLike>
    void poll(WebSocketLike& transport) {
        transport.poll();

        const bool connected = transport.hasConnection();
        const auto generation = connected ? currentConnectionGeneration(transport) : 0;
        if (!connected) {
            _activeConnectionGeneration = 0;
        } else if (_activeConnectionGeneration != generation) {
            _activeConnectionGeneration = generation;
            _helloSent = false;
            _identified = false;
            _sid = makeSessionId();
        }

        if (connected) {
            sendHelloOnce();
        }
    }

    void onBytes(const Byte* data, std::size_t size) override {
        try {
            const std::string text(reinterpret_cast<const char*>(data), size);
            const auto object = nlohmann::json::parse(text);
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
            if (_identified &&
                (op == RpcOp::Request || op == RpcOp::RequestBatch || op == RpcOp::Event ||
                 op == RpcOp::RequestResponse) &&
                !isAcceptedSid(parseSid(object))) {
                sendError(_sid, parseRequestId(object), ErrorCode::RpcPayloadInvalid, op);
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
    static RpcOp parseOp(const nlohmann::json& object) {
        const auto raw = object.at("op").get<std::int64_t>();
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

    static std::string parseSid(const nlohmann::json& object) {
        if (!object.contains("sid") || !object.at("sid").is_string()) {
            return "";
        }
        return object.at("sid").get<std::string>();
    }

    bool isAcceptedSid(const std::string& sid) const {
        return !sid.empty() && sid == _sid;
    }

    static std::uint32_t parseRequestId(const nlohmann::json& object) {
        if (!object.contains("d") || !object.at("d").is_object()) {
            return 0;
        }
        const auto& d = object.at("d");
        if (!d.contains("id")) {
            return 0;
        }
        const auto& id = d.at("id");
        if (id.is_number_unsigned()) {
            const auto raw = id.get<std::uint64_t>();
            return raw <= std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(raw)
                       : 0;
        }
        if (id.is_number_integer()) {
            const auto raw = id.get<std::int64_t>();
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

    void handleIdentify(const nlohmann::json& object) {
        const auto& d = object.at("d");
        if (!d.is_object()) {
            throw std::invalid_argument("invalid d");
        }
        const auto randomSeed = parseRandomSeed(d);
        if (const auto resumeSid = d.find("resumeSid");
            resumeSid != d.end() && resumeSid->is_string() &&
            !resumeSid->get<std::string>().empty()) {
            _sid = resumeSid->get<std::string>();
        } else {
            _sid = makeSessionId(randomSeed.value_or(0));
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
        response.bodyEncoding = RpcBodyEncoding::None;
        response.meta.sourceProtocol = SourceProtocol::JsonRpc;
        response.meta.jsonSid = sid.empty() ? _sid : sid;
        sendRpc(std::move(response));
    }

    static std::optional<std::uint32_t> parseRandomSeed(const nlohmann::json& d) {
        const auto field = d.find("randomSeed");
        if (field == d.end()) {
            return std::nullopt;
        }
        std::uint64_t raw = 0;
        if (field->is_number_unsigned()) {
            raw = field->get<std::uint64_t>();
        } else if (field->is_number_integer()) {
            const auto signedValue = field->get<std::int64_t>();
            if (signedValue < 0) {
                throw std::invalid_argument("negative randomSeed");
            }
            raw = static_cast<std::uint64_t>(signedValue);
        } else {
            throw std::invalid_argument("invalid randomSeed");
        }
        if (raw > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("randomSeed out of range");
        }
        return static_cast<std::uint32_t>(raw);
    }

    std::string makeSessionId(std::uint32_t randomSeed = 0) {
        auto mixed = randomSeed ^ (_nextSessionId++ * 0x9E3779B9U);
        if (mixed == 0) {
            mixed = _nextSessionId;
        }
        std::ostringstream out;
        out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << mixed;
        return out.str();
    }

    template <typename Transport, typename = void>
    struct HasConnectionGeneration : std::false_type {};

    template <typename Transport>
    struct HasConnectionGeneration<
        Transport,
        std::void_t<decltype(std::declval<const Transport&>().connectionGeneration())>>
        : std::true_type {};

    template <typename Transport>
    static std::uint64_t currentConnectionGeneration(const Transport& transport) {
        if constexpr (HasConnectionGeneration<Transport>::value) {
            return transport.connectionGeneration();
        } else {
            return transport.hasConnection() ? 1 : 0;
        }
    }

    AxtpCore* _core = nullptr;
    ITransport& _writer;
    std::function<void()> _pollEndpoint;
    JsonRpcEncoder _encoder;
    bool _helloSent = false;
    bool _identified = false;
    std::uint64_t _activeConnectionGeneration = 0;
    std::uint32_t _nextSessionId = 1;
    std::string _sid = makeSessionId();
};

}  // namespace axtp
