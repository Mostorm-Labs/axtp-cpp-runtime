#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/inbound/payload_sink.hpp"
#include "generated/registry_lookup.h"

namespace axtp {

class JsonRpcPayloadDecoder {
public:
    static void decode(const Byte* data,
                       std::size_t size,
                       IPayloadSink& sink,
                       SourceProtocol sourceProtocol) {
        try {
            const std::string text(reinterpret_cast<const char*>(data), size);
            const auto object = nlohmann::json::parse(text);
            const auto op = parseOp(object);
            const auto& d = object.at("d");
            if (!d.is_object()) {
                throw std::invalid_argument("invalid d");
            }

            if (op == RpcOp::Request) {
                decodeRequest(object, d, sink, sourceProtocol);
                return;
            }
            if (op == RpcOp::RequestResponse) {
                decodeResponse(object, d, op, sink, sourceProtocol);
                return;
            }
            if (op == RpcOp::RequestBatchResponse) {
                decodeResponse(object, d, op, sink, sourceProtocol);
                return;
            }
            if (op == RpcOp::Event) {
                decodeEvent(object, d, sink, sourceProtocol);
                return;
            }
            if (op == RpcOp::Identify || op == RpcOp::Reidentify ||
                op == RpcOp::Identified || op == RpcOp::Hello) {
                decodeSessionRpc(object, d, op, sink, sourceProtocol);
                return;
            }
            if (op == RpcOp::RequestBatch) {
                decodeBatch(object, d, sink, sourceProtocol);
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

    static std::uint32_t parseRequestId(const nlohmann::json& d, bool allowZero) {
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
        if ((!allowZero && raw == 0) || raw > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("id out of range");
        }
        return static_cast<std::uint32_t>(raw);
    }

    static ErrorCode parseStatusCode(const nlohmann::json& d) {
        const auto status = d.find("status");
        if (status == d.end() || !status->is_object()) {
            return ErrorCode::Success;
        }
        const auto code = status->find("code");
        if (code == status->end()) {
            return ErrorCode::Success;
        }
        std::uint64_t raw = 0;
        if (code->is_number_unsigned()) {
            raw = code->get<std::uint64_t>();
        } else if (code->is_number_integer()) {
            const auto signedCode = code->get<std::int64_t>();
            if (signedCode < 0) {
                throw std::invalid_argument("negative status code");
            }
            raw = static_cast<std::uint64_t>(signedCode);
        } else {
            throw std::invalid_argument("invalid status code");
        }
        if (raw > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("status code out of range");
        }
        return static_cast<ErrorCode>(static_cast<std::uint16_t>(raw));
    }

    static Bytes jsonToBytes(const nlohmann::json& value) {
        const auto text = value.dump();
        return Bytes(text.begin(), text.end());
    }

    static void fillJsonMeta(RpcPayload& payload,
                             const nlohmann::json& object,
                             SourceProtocol sourceProtocol) {
        payload.meta.sourceProtocol = sourceProtocol;
        payload.meta.requestId = payload.requestId;
        payload.meta.jsonSid = parseSid(object);
    }

    static void decodeRequest(const nlohmann::json& object,
                              const nlohmann::json& d,
                              IPayloadSink& sink,
                              SourceProtocol sourceProtocol) {
        if (!d.contains("method") || !d.at("method").is_string()) {
            throw std::invalid_argument("missing method");
        }
        const auto method = d.at("method").get<std::string>();
        const auto methodId = RegistryLookup::methodIdByName(method);
        if (!methodId.has_value()) {
            RpcPayload error;
            error.encoding = RpcEncoding::Json;
            error.op = RpcOp::RequestResponse;
            error.requestId = parseRequestId(d, false);
            error.statusCode = ErrorCode::RpcMethodNotFound;
            error.bodyEncoding = RpcBodyEncoding::None;
            fillJsonMeta(error, object, sourceProtocol);
            error.meta.jsonMethodOrEventName = method;
            sink.onRpc(std::move(error));
            return;
        }

        RpcPayload request;
        request.encoding = RpcEncoding::Json;
        request.op = RpcOp::Request;
        request.requestId = parseRequestId(d, false);
        request.methodOrEventId = *methodId;
        request.bodyEncoding = RpcBodyEncoding::None;
        fillJsonMeta(request, object, sourceProtocol);
        request.meta.jsonMethodOrEventName = method;
        if (const auto params = d.find("params"); params != d.end()) {
            request.body = jsonToBytes(*params);
        }
        sink.onRpc(std::move(request));
    }

    static void decodeResponse(const nlohmann::json& object,
                               const nlohmann::json& d,
                               RpcOp op,
                               IPayloadSink& sink,
                               SourceProtocol sourceProtocol) {
        RpcPayload response;
        response.encoding = RpcEncoding::Json;
        response.op = op;
        response.requestId = parseRequestId(d, true);
        response.statusCode = parseStatusCode(d);
        response.bodyEncoding = RpcBodyEncoding::None;
        fillJsonMeta(response, object, sourceProtocol);
        if (const auto result = d.find("result"); result != d.end()) {
            response.body = jsonToBytes(*result);
        } else if (response.statusCode != ErrorCode::Success) {
            response.body = jsonToBytes(d);
        }
        sink.onRpc(std::move(response));
    }

    static void decodeEvent(const nlohmann::json& object,
                            const nlohmann::json& d,
                            IPayloadSink& sink,
                            SourceProtocol sourceProtocol) {
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
        fillJsonMeta(event, object, sourceProtocol);
        event.meta.jsonMethodOrEventName = eventName;
        if (const auto eventData = d.find("data"); eventData != d.end()) {
            event.body = jsonToBytes(*eventData);
        }
        sink.onRpc(std::move(event));
    }

    static void decodeSessionRpc(const nlohmann::json& object,
                                 const nlohmann::json& d,
                                 RpcOp op,
                                 IPayloadSink& sink,
                                 SourceProtocol sourceProtocol) {
        RpcPayload payload;
        payload.encoding = RpcEncoding::Json;
        payload.op = op;
        payload.bodyEncoding = RpcBodyEncoding::None;
        fillJsonMeta(payload, object, sourceProtocol);
        payload.body = jsonToBytes(d);
        sink.onRpc(std::move(payload));
    }

    static void decodeBatch(const nlohmann::json& object,
                            const nlohmann::json& d,
                            IPayloadSink& sink,
                            SourceProtocol sourceProtocol) {
        RpcPayload payload;
        payload.encoding = RpcEncoding::Json;
        payload.op = RpcOp::RequestBatchResponse;
        payload.requestId = parseRequestId(d, true);
        payload.statusCode = ErrorCode::RpcBatchUnsupported;
        payload.bodyEncoding = RpcBodyEncoding::None;
        fillJsonMeta(payload, object, sourceProtocol);
        payload.body = jsonToBytes(d);
        sink.onRpc(std::move(payload));
    }
};

}  // namespace axtp
