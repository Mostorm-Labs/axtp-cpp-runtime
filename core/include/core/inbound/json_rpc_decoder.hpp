#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <limits>
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
            const auto parsed = boost::json::parse(text);
            const auto& object = parsed.as_object();
            const auto op = parseOp(object);
            const auto& d = object.at("d").as_object();

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
    static RpcOp parseOp(const boost::json::object& object) {
        const auto raw = object.at("op").as_int64();
        if (raw < 0 || raw > std::numeric_limits<std::uint8_t>::max()) {
            throw std::invalid_argument("invalid op");
        }
        return static_cast<RpcOp>(static_cast<std::uint8_t>(raw));
    }

    static std::string parseSid(const boost::json::object& object) {
        if (!object.contains("sid") || !object.at("sid").is_string()) {
            return "";
        }
        return std::string(object.at("sid").as_string());
    }

    static std::uint32_t parseRequestId(const boost::json::object& d) {
        if (!d.contains("id")) {
            throw std::invalid_argument("missing id");
        }
        std::uint64_t raw = 0;
        const auto& id = d.at("id");
        if (id.is_uint64()) {
            raw = id.as_uint64();
        } else if (id.is_int64()) {
            const auto signedId = id.as_int64();
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

    static Bytes jsonToBytes(const boost::json::value& value) {
        const auto text = boost::json::serialize(value);
        return Bytes(text.begin(), text.end());
    }

    void decodeRequest(const boost::json::object& object, const boost::json::object& d) {
        if (!d.contains("method") || !d.at("method").is_string()) {
            throw std::invalid_argument("missing method");
        }
        const std::string method(d.at("method").as_string());
        const auto methodId = RegistryLookup::methodIdByName(method);
        if (!methodId.has_value()) {
            RpcPayload error;
            error.encoding = RpcEncoding::Json;
            error.op = RpcOp::RequestResponse;
            error.requestId = parseRequestId(d);
            error.statusCode = ErrorCode::RpcMethodNotFound;
            error.bodyEncoding = RpcBodyEncoding::RawBytes;
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
        request.bodyEncoding = RpcBodyEncoding::RawBytes;
        request.meta.sourceProtocol = SourceProtocol::JsonRpc;
        request.meta.requestId = request.requestId;
        request.meta.jsonSid = parseSid(object);
        request.meta.jsonMethodOrEventName = method;
        if (const auto* params = d.if_contains("params")) {
            request.body = jsonToBytes(*params);
        }
        _sink.onRpc(std::move(request));
    }

    void decodeEvent(const boost::json::object& object, const boost::json::object& d) {
        if (!d.contains("event") || !d.at("event").is_string()) {
            throw std::invalid_argument("missing event");
        }
        const std::string eventName(d.at("event").as_string());
        const auto eventId = RegistryLookup::eventIdByName(eventName);
        if (!eventId.has_value()) {
            return;
        }
        RpcPayload event;
        event.encoding = RpcEncoding::Json;
        event.op = RpcOp::Event;
        event.requestId = 0;
        event.methodOrEventId = *eventId;
        event.bodyEncoding = RpcBodyEncoding::RawBytes;
        event.meta.sourceProtocol = SourceProtocol::JsonRpc;
        event.meta.jsonSid = parseSid(object);
        event.meta.jsonMethodOrEventName = eventName;
        if (const auto* data = d.if_contains("data")) {
            event.body = jsonToBytes(*data);
        }
        _sink.onRpc(std::move(event));
    }

    void
    decodeSessionRpc(const boost::json::object& object, const boost::json::object& d, RpcOp op) {
        RpcPayload payload;
        payload.encoding = RpcEncoding::Json;
        payload.op = op;
        payload.bodyEncoding = RpcBodyEncoding::RawBytes;
        payload.meta.sourceProtocol = SourceProtocol::JsonRpc;
        payload.meta.jsonSid = parseSid(object);
        payload.body = jsonToBytes(d);
        _sink.onRpc(std::move(payload));
    }

    void decodeBatch(const boost::json::object& object, const boost::json::object& d) {
        RpcPayload payload;
        payload.encoding = RpcEncoding::Json;
        payload.op = RpcOp::RequestBatchResponse;
        payload.requestId = parseRequestId(d);
        payload.statusCode = ErrorCode::RpcBatchUnsupported;
        payload.bodyEncoding = RpcBodyEncoding::RawBytes;
        payload.meta.sourceProtocol = SourceProtocol::JsonRpc;
        payload.meta.requestId = payload.requestId;
        payload.meta.jsonSid = parseSid(object);
        payload.body = jsonToBytes(d);
        _sink.onRpc(std::move(payload));
    }

    IPayloadSink& _sink;
};

}  // namespace axtp
