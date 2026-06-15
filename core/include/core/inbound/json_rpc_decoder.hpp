#pragma once

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/inbound/payload_decoder.hpp"
#include "generated/registry_lookup.h"
#include "io/byte_sink.hpp"

namespace axtp {

class JsonRpcDecoder : public IByteSink {
public:
    explicit JsonRpcDecoder(IPayloadSink& sink)
        : _sink(sink) {}

    // WebSocketJsonRpc mode receives one complete WebSocket text message per call.
    // It is not a byte-stream parser and must not be fed arbitrary TCP chunks.
    void onBytes(const Byte* data, std::size_t size) override {
        try {
            const std::string text(reinterpret_cast<const char*>(data), size);
            const auto object = nlohmann::json::parse(text);
            const auto op = parseOp(object);
            const auto& d = object.at("d");
            if (!d.is_object()) {
                throw std::invalid_argument("invalid d");
            }

            if (op == RpcOp::Request) {
                decodeRequest(object, d);
                return;
            }
            if (op == RpcOp::Event) {
                decodeEvent(object, d);
                return;
            }
            if (op == RpcOp::Identify || op == RpcOp::Reidentify) {
                decodeSessionRpc(object, d, op);
                return;
            }
            if (op == RpcOp::RequestBatch) {
                decodeBatch(object, d);
            }
        } catch (const std::exception&) {
        }
    }

private:
    static RpcOp parseOp(const nlohmann::json& object) {
        const auto raw = object.at("op").get<std::int64_t>();
        if (raw < 0 || raw > std::numeric_limits<std::uint8_t>::max()) {
            throw std::invalid_argument("invalid op");
        }
        return static_cast<RpcOp>(static_cast<std::uint8_t>(raw));
    }

    static std::string parseSid(const nlohmann::json& object) {
        if (!object.contains("sid") || !object.at("sid").is_string()) {
            return "";
        }
        return object.at("sid").get<std::string>();
    }

    static std::uint32_t parseRequestId(const nlohmann::json& d) {
        if (!d.contains("id")) {
            throw std::invalid_argument("missing id");
        }
        std::uint64_t raw = 0;
        const auto& id = d.at("id");
        if (id.is_number_unsigned()) {
            raw = id.get<std::uint64_t>();
        } else if (id.is_number_integer()) {
            const auto signedId = id.get<std::int64_t>();
            if (signedId < 0) {
                throw std::invalid_argument("negative id");
            }
            raw = static_cast<std::uint64_t>(signedId);
        } else {
            throw std::invalid_argument("invalid id");
        }
        if (raw == 0 || raw > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("id out of range");
        }
        return static_cast<std::uint32_t>(raw);
    }

    static Bytes jsonToBytes(const nlohmann::json& value) {
        const auto text = value.dump();
        return Bytes(text.begin(), text.end());
    }

    void decodeRequest(const nlohmann::json& object, const nlohmann::json& d) {
        if (!d.contains("method") || !d.at("method").is_string()) {
            throw std::invalid_argument("missing method");
        }
        const auto method = d.at("method").get<std::string>();
        const auto methodId = RegistryLookup::methodIdByName(method);
        if (!methodId.has_value()) {
            RpcPayload error;
            error.encoding = RpcEncoding::Json;
            error.op = RpcOp::RequestResponse;
            error.requestId = parseRequestId(d);
            error.statusCode = ErrorCode::RpcMethodNotFound;
            error.bodyEncoding = RpcBodyEncoding::None;
            error.meta.sourceProtocol = SourceProtocol::JsonRpc;
            error.meta.jsonSid = parseSid(object);
            error.meta.jsonMethodOrEventName = method;
            _sink.onRpc(std::move(error));
            return;
        }

        RpcPayload request;
        request.encoding = RpcEncoding::Json;
        request.op = RpcOp::Request;
        request.requestId = parseRequestId(d);
        request.methodOrEventId = *methodId;
        request.bodyEncoding = RpcBodyEncoding::None;
        request.meta.sourceProtocol = SourceProtocol::JsonRpc;
        request.meta.requestId = request.requestId;
        request.meta.jsonSid = parseSid(object);
        request.meta.jsonMethodOrEventName = method;
        if (const auto params = d.find("params"); params != d.end()) {
            request.body = jsonToBytes(*params);
        }
        _sink.onRpc(std::move(request));
    }

    void decodeEvent(const nlohmann::json& object, const nlohmann::json& d) {
        if (!d.contains("event") || !d.at("event").is_string()) {
            throw std::invalid_argument("missing event");
        }
        const auto eventName = d.at("event").get<std::string>();
        const auto eventId = RegistryLookup::eventIdByName(eventName);
        if (!eventId.has_value()) {
            return;
        }
        RpcPayload event;
        event.encoding = RpcEncoding::Json;
        event.op = RpcOp::Event;
        event.requestId = 0;
        event.methodOrEventId = *eventId;
        event.bodyEncoding = RpcBodyEncoding::None;
        event.meta.sourceProtocol = SourceProtocol::JsonRpc;
        event.meta.jsonSid = parseSid(object);
        event.meta.jsonMethodOrEventName = eventName;
        if (const auto eventData = d.find("data"); eventData != d.end()) {
            event.body = jsonToBytes(*eventData);
        }
        _sink.onRpc(std::move(event));
    }

    void
    decodeSessionRpc(const nlohmann::json& object, const nlohmann::json& d, RpcOp op) {
        RpcPayload payload;
        payload.encoding = RpcEncoding::Json;
        payload.op = op;
        payload.bodyEncoding = RpcBodyEncoding::None;
        payload.meta.sourceProtocol = SourceProtocol::JsonRpc;
        payload.meta.jsonSid = parseSid(object);
        payload.body = jsonToBytes(d);
        _sink.onRpc(std::move(payload));
    }

    void decodeBatch(const nlohmann::json& object, const nlohmann::json& d) {
        RpcPayload payload;
        payload.encoding = RpcEncoding::Json;
        payload.op = RpcOp::RequestBatchResponse;
        payload.requestId = parseRequestId(d);
        payload.statusCode = ErrorCode::RpcBatchUnsupported;
        payload.bodyEncoding = RpcBodyEncoding::None;
        payload.meta.sourceProtocol = SourceProtocol::JsonRpc;
        payload.meta.requestId = payload.requestId;
        payload.meta.jsonSid = parseSid(object);
        payload.body = jsonToBytes(d);
        _sink.onRpc(std::move(payload));
    }

    IPayloadSink& _sink;
};

}  // namespace axtp
