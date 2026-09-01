#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "core/protocol/wire/payload_sink.hpp"
#include "core/protocol/generated/registry_lookup.h"
#include "core/protocol/wire/websocket_json_rpc/endpoint_metadata_codec.hpp"

namespace axtp {

class JsonRpcPayloadDecoder {
public:
    using NameLookup = std::function<std::optional<std::uint32_t>(std::string_view)>;

    static void decode(const Byte* data,
                       std::size_t size,
                       IPayloadSink& sink,
                       SourceProtocol sourceProtocol,
                       const NameLookup& methodLookup = {},
                       const NameLookup& eventLookup = {}) {
        try {
            const std::string text(reinterpret_cast<const char*>(data), size);
            const auto object = nlohmann::json::parse(text);
            const auto op = parseOp(object);
            const auto& d = object.at("d");
            if (!d.is_object()) {
                throw std::invalid_argument("invalid d");
            }

            if (op == RpcOp::Request) {
                decodeRequest(object, d, sink, sourceProtocol, methodLookup);
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
                decodeEvent(object, d, sink, sourceProtocol, eventLookup);
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
    static std::optional<std::uint32_t> resolveName(std::string_view name,
                                                    const NameLookup& lookup,
                                                    bool eventName) {
        if (lookup) {
            if (auto id = lookup(name)) {
                return id;
            }
        }
        if (eventName) {
            if (auto id = RegistryLookup::eventIdByName(name)) {
                return static_cast<std::uint32_t>(*id);
            }
        } else {
            if (auto id = RegistryLookup::methodIdByName(name)) {
                return static_cast<std::uint32_t>(*id);
            }
        }
        return std::nullopt;
    }

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
            throw std::invalid_argument("invalid status");
        }
        const auto ok = status->find("ok");
        if (ok == status->end() || !ok->is_boolean()) {
            throw std::invalid_argument("invalid status ok");
        }
        const auto code = status->find("code");
        if (code == status->end()) {
            throw std::invalid_argument("missing status code");
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
        const auto okValue = ok->get<bool>();
        if ((okValue && raw != 0) || (!okValue && raw == 0)) {
            throw std::invalid_argument("inconsistent status");
        }
        return static_cast<ErrorCode>(static_cast<std::uint16_t>(raw));
    }

    static std::optional<std::uint32_t> parseRandomSeed(const nlohmann::json& d) {
        const auto randomSeed = d.find("randomSeed");
        if (randomSeed == d.end()) {
            return std::nullopt;
        }
        std::uint64_t raw = 0;
        if (randomSeed->is_number_unsigned()) {
            raw = randomSeed->get<std::uint64_t>();
        } else if (randomSeed->is_number_integer()) {
            const auto signedSeed = randomSeed->get<std::int64_t>();
            if (signedSeed < 0) {
                throw std::invalid_argument("negative randomSeed");
            }
            raw = static_cast<std::uint64_t>(signedSeed);
        } else {
            throw std::invalid_argument("invalid randomSeed");
        }
        if (raw > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("randomSeed out of range");
        }
        return static_cast<std::uint32_t>(raw);
    }

    static Bytes jsonToBytes(const nlohmann::json& value) {
        const auto text = value.dump();
        return Bytes(text.begin(), text.end());
    }

    static bool hasUnsupportedTransitionRpcVersion(const nlohmann::json& d,
                                                   const char* fieldName) {
        const auto field = d.find(fieldName);
        if (field == d.end()) {
            return false;
        }
        if (!field->is_number_integer() && !field->is_number_unsigned()) {
            return true;
        }
        return field->get<std::int64_t>() != 1;
    }

    static void fillJsonMeta(RpcPayload& payload,
                             const nlohmann::json& object,
                             SourceProtocol sourceProtocol) {
        payload.meta.sourceProtocol = sourceProtocol;
        payload.meta.requestId = payload.requestId;
        payload.meta.jsonSid = parseSid(object);
        payload.meta.endpoint = decodeEndpointMetadata(object);
    }

    static void decodeRequest(const nlohmann::json& object,
                              const nlohmann::json& d,
                              IPayloadSink& sink,
                              SourceProtocol sourceProtocol,
                              const NameLookup& methodLookup) {
        if (!d.contains("method") || !d.at("method").is_string()) {
            throw std::invalid_argument("missing method");
        }
        const auto method = d.at("method").get<std::string>();
        const auto methodId = resolveName(method, methodLookup, false);
        if (!methodId.has_value()) {
            RpcPayload error;
            error.encoding = RpcEncoding::Json;
            error.op = RpcOp::RequestResponse;
            error.requestId = parseRequestId(d, false);
            error.statusCode = ErrorCode::RpcMethodNotFound;
            error.bodyEncoding = RpcBodyEncoding::None;
            fillJsonMeta(error, object, sourceProtocol);
            error.meta.endpoint = responseEndpointMetadata(error.meta.endpoint);
            error.meta.jsonMethodOrEventName = method;
            error.meta.localGeneratedResponse = true;
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
            if (response.statusCode != ErrorCode::Success) {
                throw std::invalid_argument("error response carries result");
            }
            response.body = jsonToBytes(*result);
        } else if (response.statusCode != ErrorCode::Success) {
            response.body = jsonToBytes(d);
        }
        sink.onRpc(std::move(response));
    }

    static void decodeEvent(const nlohmann::json& object,
                            const nlohmann::json& d,
                            IPayloadSink& sink,
                            SourceProtocol sourceProtocol,
                            const NameLookup& eventLookup) {
        if (!d.contains("event") || !d.at("event").is_string()) {
            throw std::invalid_argument("missing event");
        }
        const auto eventName = d.at("event").get<std::string>();
        const auto eventId = resolveName(eventName, eventLookup, true);
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
        if (hasUnsupportedTransitionRpcVersion(d, "rpcVersion") ||
            hasUnsupportedTransitionRpcVersion(d, "negotiatedRpcVersion")) {
            return;
        }
        if (op == RpcOp::Identify || op == RpcOp::Reidentify) {
            if (auto randomSeed = parseRandomSeed(d)) {
                payload.meta.hasRandomSeed = true;
                payload.meta.randomSeed = *randomSeed;
            }
        }
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
        payload.meta.endpoint = responseEndpointMetadata(payload.meta.endpoint);
        payload.meta.localGeneratedResponse = true;
        payload.body = jsonToBytes(d);
        sink.onRpc(std::move(payload));
    }
};

}  // namespace axtp
